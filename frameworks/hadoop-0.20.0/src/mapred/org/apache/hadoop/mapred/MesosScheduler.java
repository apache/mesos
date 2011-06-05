package org.apache.hadoop.mapred;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.IOException;
import java.util.Collection;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hadoop.io.ObjectWritable;

import mesos.ExecutorInfo;
import mesos.FrameworkMessage;
import mesos.MesosSchedulerDriver;
import mesos.SchedulerDriver;
import mesos.SlaveOffer;
import mesos.SlaveOfferVector;
import mesos.StringMap;
import mesos.TaskDescription;
import mesos.TaskDescriptionVector;
import mesos.TaskState;

class MesosScheduler extends mesos.Scheduler {
  public static final Log LOG =
    LogFactory.getLog(MesosScheduler.class);

  private final JobTracker jobTracker;
  private JobConf conf;

  private SchedulerDriver driver;
  
  // Thread pool for responding to RPCs (because several tasktrackers may
  // make calls in parallel)
  private ExecutorService rpcThreadPool;
  
  // Counts of active map slots and reduce slots on each slave
  private Map<String, Integer> mapSlots = new HashMap<String, Integer>();
  private Map<String, Integer> reduceSlots = new HashMap<String, Integer>();
  
  // Maps from Mesos each task ID to the slave it's on and its task type
  private Map<Integer, String> taskIdToSlave = new HashMap<Integer, String>();
  private Map<Integer, Boolean> taskIdIsMap = new HashMap<Integer, Boolean>();
  
  // Time to wait for slot offer on node with local data for maps
  private long localityWait;

  // Atomic integer used to get task IDs
  private AtomicInteger nextTaskId = new AtomicInteger(0);

  // Total number of map slots active; we make sure we have some of these at
  // all times using the minActiveMaps/minactiveReduces parameters in order
  // to ensure that job setup and cleanup are fast
  private int activeMaps = 0;
  private int activeReduces = 0;

  // Min numbers of active maps and reduces to hold
  private int minActiveMaps = 0;
  private int minActiveReduces = 0;

  public MesosScheduler(JobTracker jobTracker) throws IOException {
    this.jobTracker = jobTracker;
    this.conf = jobTracker.conf;
    int handlerCount = conf.getInt("mapred.job.tracker.handler.count", 10);
    rpcThreadPool = Executors.newFixedThreadPool(handlerCount);
    localityWait = conf.getLong("mapred.mesos.localitywait", 5000);
    minActiveMaps = conf.getInt("mapred.mesos.minActiveMaps", 0);
    minActiveReduces = conf.getInt("mapred.mesos.minActiveReduces", 0);
  }

  @Override
  public String getFrameworkName(SchedulerDriver driver) {
    return "Hadoop: " + jobTracker.getTrackerIdentifier() +
           " (RPC port: " + jobTracker.port + "," +
           " web UI port: " + jobTracker.infoPort + ")";
  }

  @Override
  public ExecutorInfo getExecutorInfo(SchedulerDriver driver) {
    try {
      String execPath = new File("bin/mesos-executor").getCanonicalPath();
      byte[] initArg = conf.get("mapred.job.tracker").getBytes("US-ASCII");
      return new ExecutorInfo(execPath, initArg);
    } catch (IOException e) {
      throw new RuntimeException(e);
    }
  }

  @Override
  public void resourceOffer(
      SchedulerDriver driver, String offerId, SlaveOfferVector offers) {
    LOG.info("Got slot offer " + offerId);
    TaskDescriptionVector tasks = new TaskDescriptionVector();
    for (int i = 0; i < offers.size(); i++) {
      TaskDescription task = scheduleSingleSlave(offers.get(i));
      if (task != null)
        tasks.add(task);
    }
    StringMap params = new StringMap();
    params.set("timeout", "1");
    driver.replyToOffer(offerId, tasks, params);
  }

  Pattern CPUS = Pattern.compile(".*cpus=([0-9]+).*",
                                 Pattern.MULTILINE | Pattern.DOTALL);
  Pattern MEM = Pattern.compile(".*mem=([0-9]+).*",
                                Pattern.MULTILINE | Pattern.DOTALL);

  public TaskDescription scheduleSingleSlave(SlaveOffer slot) {
    try {
      LOG.info("Examining slot on " + slot.getHost());

      // Make sure it's got enough resources
      int desiredCpus = conf.getInt("mapred.mesos.desired.cpus", 1);
      long desiredMem = conf.getLong(
          "mapred.mesos.desired.mem", 1024L * 1024L * 1024L);

      int offeredCpus = Integer.parseInt(slot.getParams().get("cpus"));
      long offeredMem = Long.parseLong(slot.getParams().get("mem"));

      LOG.info("offeredCpus = " + offeredCpus + ", " +
               "offeredMem = " + offeredMem);

      if (offeredCpus < desiredCpus || offeredMem < desiredMem) {
        LOG.info("Refusing it due to too few resources");
        return null;
      }

      String slaveId = slot.getSlaveId();
      Integer curMapSlots = mapSlots.get(slaveId);
      if (curMapSlots == null)
        curMapSlots = 0;
      Integer curReduceSlots = reduceSlots.get(slaveId);
      if (curReduceSlots == null)
        curReduceSlots = 0;
      
      boolean haveMaps = activeMaps < minActiveMaps || canLaunchMap(slot.getHost());
      boolean haveReduces = activeReduces < minActiveReduces || canLaunchReduce();
      
      String taskType = null;
      
      if (!haveMaps && !haveReduces) {
        LOG.info("Refusing it due to lack of suitable tasks");
        return null;
      } else if (haveMaps && !haveReduces) {
        taskType = "map";
      } else if (haveReduces && !haveMaps) {
        taskType = "reduce";
      } else {
        float desiredMaxMaps = conf.getInt(
            "mapred.tasktracker.map.tasks.maximum", 2);
        float desiredMaxReduces = conf.getInt(
            "mapred.tasktracker.reduce.tasks.maximum", 2);
        float mapRatio = curMapSlots / desiredMaxMaps;
        float reduceRatio = curReduceSlots / desiredMaxReduces;
        if (reduceRatio < mapRatio)
          taskType = "reduce";
        else
          taskType = "map";
      }

      // Create the task
      int taskId = nextTaskId.getAndIncrement();
      String name = taskType + " " + taskId;
      StringMap myParams = new StringMap();
      myParams.set("cpus", "" + desiredCpus);
      myParams.set("mem", "" + desiredMem);
      byte[] data = taskType.getBytes();
      TaskDescription td = new TaskDescription(
          taskId, slot.getSlaveId(), name, myParams, data);
      LOG.info("Accepting it to launch a " + taskType);
      
      // Increase slot counts to properly schedule tasks on next heartbeat;
      // also remember task type and location for bookkeeping
      if (taskType.equals("map")) {
        mapSlots.put(slaveId, curMapSlots + 1);
        taskIdIsMap.put(taskId, true);
        taskIdToSlave.put(taskId, slaveId);
        activeMaps++;
      }
      else {
        reduceSlots.put(slaveId, curReduceSlots + 1);
        taskIdIsMap.put(taskId, false);
        taskIdToSlave.put(taskId, slaveId);
        activeReduces++;
      }

      // Return the task
      return td;
    } catch(Exception e) {
      e.printStackTrace();
      return null;
    }
  }

  @Override
  public void statusUpdate(SchedulerDriver driver, mesos.TaskStatus status) {
    // Decrease active slot counts to properly schedule tasks
    TaskState state = status.getState();
    if (state == TaskState.TASK_FINISHED || state == TaskState.TASK_FAILED ||
        state == TaskState.TASK_KILLED || state == TaskState.TASK_LOST) {
      int taskId = status.getTaskId();
      String slaveId = taskIdToSlave.get(taskId);
      boolean isMap = taskIdIsMap.get(taskId);
      if (isMap) {
        mapSlots.put(slaveId, mapSlots.get(slaveId) - 1);
        activeMaps--;
      } else {
        reduceSlots.put(slaveId, reduceSlots.get(slaveId) - 1);
        activeReduces--;
      }
      taskIdToSlave.remove(taskId);
      taskIdIsMap.remove(taskId);
    }
  }

  @Override
  public void error(SchedulerDriver driver, int code, String message) {
    JobTracker.LOG.fatal("Mesos error: " + message +
        " (error code: " + code + ")");
    shutdownTracker();
  }

  private void shutdownTracker() {
    try {
      jobTracker.stopTracker();
    } catch (IOException e) {
      JobTracker.LOG.error("Error while shutting down JobTracker", e);
      System.exit(1);
    }
  }
  
  @Override
  public void frameworkMessage(SchedulerDriver driver,
                               final FrameworkMessage message) {
    //LOG.info("Received framework message, len = " + message.getData().length);
    // Copy slaveId and data because the message will go away
    // TODO: make the C++ API not automatically delete the message
    final String slaveId = message.getSlaveId();
    final byte[] data = message.getData();
    rpcThreadPool.execute(new Runnable() {
      public void run() {
        handleRPC(slaveId, data);
      }
    });
  }
  
  boolean lastMapWasLocal = true;
  long timeWaitedForLocalMap = 0;
  long lastCanLaunchMapTime = -1;
  
  // TODO: Make this return a count instead of a boolean?
  // TODO: Cache result for some time so we don't iterate through all jobs
  // and tasks every time we get a slot offer
  private boolean canLaunchMap(String host) {
    synchronized (jobTracker) {
      long now = System.currentTimeMillis();
      if (lastCanLaunchMapTime == -1)
        lastCanLaunchMapTime = now;
      int maxLevel; // Cache level to search for maps in
      if (lastMapWasLocal) {
        timeWaitedForLocalMap += now - lastCanLaunchMapTime;
        if (timeWaitedForLocalMap >= localityWait) {
          maxLevel = Integer.MAX_VALUE;
        } else {
          maxLevel = 1;
        }
      } else {
        maxLevel = Integer.MAX_VALUE;
      }
      lastCanLaunchMapTime = now;
      Collection<JobInProgress> jobs = jobTracker.jobs.values();
      for (JobInProgress job: jobs) {
        int state = job.getStatus().getRunState();
        if (state == JobStatus.PREP || (state == JobStatus.RUNNING && 
            job.hasMapToLaunch(host, maxLevel))) {
          return true;
        }
      }
    }
    return false;
  }
  
  // TODO: Make this return a count instead of a boolean?
  // TODO: Cache result for some time so we don't iterate through all jobs
  // and tasks every time we get a slot offer
  private boolean canLaunchReduce() {
    synchronized (jobTracker) {
      Collection<JobInProgress> jobs = jobTracker.jobs.values();
      for (JobInProgress job: jobs) {
        int state = job.getStatus().getRunState();
        if (state == JobStatus.RUNNING && job.hasReduceToLaunch()) {
          return true;
        }
      }
    }
    return false;
  }

  private void handleRPC(String slaveId, byte[] data) {
    try {
      DataInputStream in = new DataInputStream(
          new ByteArrayInputStream(data));
      //LOG.info("In handleRPC, message length = " + data.length);
      int rpcId = in.readInt();
      String method = in.readUTF();
      //LOG.info("Responding to " + method + " from executor on " + slaveId);
      int numArgs = in.readInt();
      Object[] args = new Object[numArgs];
      for (int i = 0; i < numArgs; i++) {
        ObjectWritable writable = new ObjectWritable();
        writable.setConf(conf);
        writable.readFields(in);
        args[i] = writable.get();
      }
      
      if (method.equals("getBuildVersion")) {
        sendRPCResponse(slaveId, rpcId, jobTracker.getBuildVersion());
      } else if (method.equals("getFilesystemName")) {
        sendRPCResponse(slaveId, rpcId, jobTracker.getFilesystemName());
      } else if (method.equals("getSystemDir")) {
        sendRPCResponse(slaveId, rpcId, jobTracker.getSystemDir());
      } else if (method.equals("heartbeat")) {
        TaskTrackerStatus taskTrackerStatus = (TaskTrackerStatus) args[0];
        HeartbeatResponse response = jobTracker.heartbeat(
            taskTrackerStatus,
            (Boolean) args[1], 
            (Boolean) args[2], 
            (Boolean) args[3], 
            (Short) args[4]);
        // Check whether we are launched a local task
        outer: for (TaskTrackerAction action: response.actions) {
          if (action instanceof LaunchTaskAction) {
            LaunchTaskAction lta = (LaunchTaskAction) action;
            Task task = lta.getTask();
            if (task.isMapOrReduce() && task.isMapTask()) {
              JobInProgress job = jobTracker.getJob(task.getJobID());
              if (job != null) {
                TaskInProgress tip = job.maps[task.getPartition()];
                lastMapWasLocal = false;
                for (String location: tip.getSplitLocations()) {
                  if (location.equals(taskTrackerStatus.getHost())) {
                    //LOG.info("Detected local map!!!!!!");
                    lastMapWasLocal = true;
                    timeWaitedForLocalMap = 0;
                    break outer;
                  }
                }
              }
            }
          }
        }
        sendRPCResponse(slaveId, rpcId, response);
      } else if (method.equals("reportTaskTrackerError")) {
        jobTracker.reportTaskTrackerError(
            (String) args[0], (String) args[1], (String) args[2]);
        // reportTaskTrackerError is a void method, so just return 0
        sendRPCResponse(slaveId, rpcId, 0);
      } else if (method.equals("getTaskCompletionEvents")) {
        sendRPCResponse(slaveId, rpcId, jobTracker.getTaskCompletionEvents(
            (JobID) args[0], (Integer) args[1], (Integer) args[2]));
      } else if (method.equals("getProtocolVersion")) {
        sendRPCResponse(slaveId, rpcId, jobTracker.getProtocolVersion(
            (String) args[0], (Long) args[2]));
      } else {
        LOG.warn("Unknown RPC method: " + method + ", ignoring it");
      }
    } catch (Exception e) {
      e.printStackTrace();
      driver.stop();
      shutdownTracker();
    }
  }

  private void sendRPCResponse(String slaveId, int rpcId,
      Object response) throws IOException {
    ByteArrayOutputStream bos = new ByteArrayOutputStream();
    DataOutputStream out = new DataOutputStream(bos);
    out.writeInt(rpcId);
    // Set declared class of wrappers to to primitive type to work 
    // with ObjectWritable
    Class<? extends Object> declaredClass = response.getClass();
    if (declaredClass == Boolean.class)
      declaredClass = Boolean.TYPE;
    else if (declaredClass == Short.class)
      declaredClass = Short.TYPE;
    else if (declaredClass == Integer.class)
      declaredClass = Integer.TYPE;
    else if (declaredClass == Long.class)
      declaredClass = Long.TYPE;
    // Use ObjectWritable to serialize result
    new ObjectWritable(declaredClass, response).write(out);
    byte[] bytes = bos.toByteArray();
    FrameworkMessage msg = new FrameworkMessage(slaveId, 0, bytes);
    driver.sendFrameworkMessage(msg);
  }

  public void run(String master) {
    driver = new MesosSchedulerDriver(this, master);
    driver.run();
  }
}
