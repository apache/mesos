package org.apache.hadoop.mapred;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import org.apache.commons.httpclient.HttpHost;
import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.mapreduce.server.jobtracker.TaskTracker;
import org.apache.mesos.MesosSchedulerDriver;
import org.apache.mesos.Protos;
import org.apache.mesos.Protos.CommandInfo;
import org.apache.mesos.Protos.ExecutorID;
import org.apache.mesos.Protos.ExecutorInfo;
import org.apache.mesos.Protos.FrameworkID;
import org.apache.mesos.Protos.FrameworkInfo;
import org.apache.mesos.Protos.MasterInfo;
import org.apache.mesos.Protos.Offer;
import org.apache.mesos.Protos.OfferID;
import org.apache.mesos.Protos.Resource;
import org.apache.mesos.Protos.SlaveID;
import org.apache.mesos.Protos.TaskID;
import org.apache.mesos.Protos.TaskInfo;
import org.apache.mesos.Protos.TaskState;
import org.apache.mesos.Protos.Value;
import org.apache.mesos.Scheduler;
import org.apache.mesos.SchedulerDriver;

import static org.apache.hadoop.util.StringUtils.join;

public class MesosScheduler extends TaskScheduler implements Scheduler {
  public static final Log LOG = LogFactory.getLog(MesosScheduler.class);

  private SchedulerDriver driver;
  private TaskScheduler taskScheduler;
  private JobTracker jobTracker;
  private Configuration conf;

  // This is the memory overhead for a jvm process. This needs to be added
  // to a jvm process's resource requirement, in addition to its heap size.
  private static final int JVM_MEM_OVERHEAD = 256; // 256 MB.

  // TODO(vinod): Consider parsing the slot memory from the configuration jvm
  // heap options (e.g: mapred.child.java.opts).

  // NOTE: It appears that there's no real resource requirements for a
  // map / reduce slot. We therefore define a default slot as:
  // 0.2 cores.
  // 512 MB memory.
  // 1 GB of disk space.
  private static final double SLOT_CPUS_DEFAULT = 0.2; // 0.2 cores.
  private static final int SLOT_DISK_DEFAULT = 1024; // 1 GB.
  private static final int SLOT_JVM_HEAP_DEFAULT = 256; // MB.

  private static final double TASKTRACKER_CPUS = 1.0; // 1 core.
  private static final int TASKTRACKER_JVM_HEAP = 1024; // 1 GB.
  private static final int TASKTRACKER_MEM =
      TASKTRACKER_JVM_HEAP + JVM_MEM_OVERHEAD;

  // The default behavior in Hadoop is to use 4 slots per TaskTracker:
  private static final int MAP_SLOTS_DEFAULT = 2;
  private static final int REDUCE_SLOTS_DEFAULT = 2;

  // Count of the launched trackers for TaskID generation.
  private long launchedTrackers = 0;

  // Maintains a mapping from {tracker host:port -> MesosTracker}.
  // Used for tracking the slots of each TaskTracker and the corresponding
  // Mesos TaskID.
  private Map<HttpHost, MesosTracker> mesosTrackers =
      new HashMap<HttpHost, MesosTracker>();

  private JobInProgressListener jobListener = new JobInProgressListener() {
    @Override
    public void jobAdded(JobInProgress job) throws IOException {
      LOG.info("Added job " + job.getJobID());
    }

    @Override
    public void jobRemoved(JobInProgress job) {
      LOG.info("Removed job " + job.getJobID());
    }

    @Override
    public void jobUpdated(JobChangeEvent event) {
      synchronized (MesosScheduler.this) {
        JobInProgress job = event.getJobInProgress();

        // If the job is complete, kill all the corresponding idle TaskTrackers.
        if (!job.isComplete())
          return;

        LOG.info("Completed job : " + job.getJobID());

        List<TaskInProgress> completed = new ArrayList<TaskInProgress>();

        // Map tasks.
        completed.addAll(job.reportTasksInProgress(true, true));

        // Reduce tasks.
        completed.addAll(job.reportTasksInProgress(false, true));

        for (TaskInProgress task : completed) {
          for (TaskStatus status : task.getTaskStatuses()) {
            LOG.info("Removing completed task : " + status.getTaskID()
                + " of tracker " + status.getTaskTracker());

            // Make a copy to iterate over keys and delete values.
            Set<HttpHost> trackers = new HashSet<HttpHost>(
                mesosTrackers.keySet());

            // Remove the task from the map.
            for (HttpHost tracker : trackers) {
              MesosTracker mesosTracker = mesosTrackers.get(tracker);

              if (!mesosTracker.active) {
                LOG.warn("Ignoring TaskTracker: " + tracker
                    + " because it might not have sent a hearbeat");
                continue;
              }

              mesosTracker.hadoopTasks.remove(status.getTaskID());

              // If this TaskTracker doesn't have any running tasks, kill it.
              if (mesosTracker.hadoopTasks.isEmpty()) {
                LOG.info("Killing Mesos task: " + mesosTracker.taskId
                    + " on host " + mesosTracker.host);

                driver.killTask(mesosTracker.taskId);
                mesosTrackers.remove(tracker);
              }
            }
          }
        }
      }
    }
  };

  public MesosScheduler() {}

  // TaskScheduler methods.
  @Override
  public synchronized void start() throws IOException {
    conf = getConf();
    String taskTrackerClass = conf.get("mapred.mesos.taskScheduler",
        "org.apache.hadoop.mapred.JobQueueTaskScheduler");

    try {
      taskScheduler =
          (TaskScheduler) Class.forName(taskTrackerClass).newInstance();
      taskScheduler.setConf(conf);
      taskScheduler.setTaskTrackerManager(taskTrackerManager);
    } catch (ClassNotFoundException e) {
      LOG.fatal("Failed to initialize the TaskScheduler", e);
      System.exit(1);
    } catch (InstantiationException e) {
      LOG.fatal("Failed to initialize the TaskScheduler", e);
      System.exit(1);
    } catch (IllegalAccessException e) {
      LOG.fatal("Failed to initialize the TaskScheduler", e);
      System.exit(1);
    }

    // Add the job listener to get job related updates.
    taskTrackerManager.addJobInProgressListener(jobListener);

    LOG.info("Starting MesosScheduler");
    jobTracker = (JobTracker) super.taskTrackerManager;

    String master = conf.get("mapred.mesos.master", "local");

    try {
      FrameworkInfo frameworkInfo = FrameworkInfo
          .newBuilder()
          .setUser("")
          .setName("Hadoop: (RPC port: " + jobTracker.port + ","
                   + " WebUI port: " + jobTracker.infoPort + ")").build();

      driver = new MesosSchedulerDriver(this, frameworkInfo, master);
      driver.start();
    } catch (Exception e) {
      // If the MesosScheduler can't be loaded, the JobTracker won't be useful
      // at all, so crash it now so that the user notices.
      LOG.fatal("Failed to start MesosScheduler", e);
      System.exit(1);
    }

    taskScheduler.start();
  }

  @Override
  public synchronized void terminate() throws IOException {
    try {
      LOG.info("Stopping MesosScheduler");
      driver.stop();
    } catch (Exception e) {
      LOG.error("Failed to stop Mesos scheduler", e);
    }

    taskScheduler.terminate();
  }

  @Override
  public synchronized List<Task> assignTasks(TaskTracker taskTracker)
      throws IOException {
    HttpHost tracker = new HttpHost(taskTracker.getStatus().getHost(),
        taskTracker.getStatus().getHttpPort());

    if (!mesosTrackers.containsKey(tracker)) {
      // TODO(bmahler): Consider allowing non-Mesos TaskTrackers.
      LOG.info("Unknown/exited TaskTracker: " + tracker + ". ");
      return null;
    }

    // Let the underlying task scheduler do the actual task scheduling.
    List<Task> tasks = taskScheduler.assignTasks(taskTracker);

    // The Hadoop Fair Scheduler is known to return null.
    if (tasks != null) {
      // Keep track of which TaskTracker contains which tasks.
      for (Task task : tasks) {
        LOG.info("Assigning task : " + task.getTaskID() + " to tracker "
            + tracker);
        mesosTrackers.get(tracker).hadoopTasks.add(task.getTaskID());
      }
    }

    return tasks;
  }

  @Override
  public synchronized Collection<JobInProgress> getJobs(String queueName) {
    return taskScheduler.getJobs(queueName);
  }

  @Override
  public synchronized void refresh() throws IOException {
    taskScheduler.refresh();
  }

  // Mesos Scheduler methods.
  // These are synchronized, where possible. Some of these methods need to access the
  // JobTracker, which can lead to a deadlock:
  // See: https://issues.apache.org/jira/browse/MESOS-429
  // The workaround employed here is to unsynchronize those methods needing access to
  // the JobTracker state and use explicit synchronization instead as appropriate.
  // TODO(bmahler): Provide a cleaner solution to this issue. One solution is to
  // split up the Scheduler and TaskScheduler implementations in order to break the
  // locking cycle. This would require a synchronized class to store the shared
  // state across our Scheduler and TaskScheduler implementations, and provide
  // atomic operations as needed.
  @Override
  public synchronized void registered(SchedulerDriver schedulerDriver,
      FrameworkID frameworkID, MasterInfo masterInfo) {
    LOG.info("Registered as " + frameworkID.getValue()
        + " with master " + masterInfo);
  }

  @Override
  public synchronized void reregistered(SchedulerDriver schedulerDriver,
      MasterInfo masterInfo) {
    LOG.info("Re-registered with master " + masterInfo);
  }

  // This method uses explicit synchronization in order to avoid deadlocks when
  // accessing the JobTracker.
  @Override
  public void resourceOffers(SchedulerDriver schedulerDriver,
      List<Offer> offers) {
    // Before synchronizing, we pull all needed information from the JobTracker.
    final HttpHost jobTrackerAddress = new HttpHost(jobTracker.getHostname(),
        jobTracker.getTrackerPort());

    final Collection<TaskTrackerStatus> taskTrackers = jobTracker.taskTrackers();

    final List<JobInProgress> jobsInProgress = new ArrayList<JobInProgress>();
    for (JobStatus status : jobTracker.jobsToComplete()) {
      jobsInProgress.add(jobTracker.getJob(status.getJobID()));
    }

    synchronized (this) {
      // Compute the number of pending maps and reduces.
      int pendingMaps = 0;
      int pendingReduces = 0;
      for (JobInProgress progress : jobsInProgress) {
        pendingMaps += progress.pendingMaps();
        pendingReduces += progress.pendingReduces();
      }

      // Mark active (heartbeated) TaskTrackers and compute idle slots.
      int idleMapSlots = 0;
      int idleReduceSlots = 0;
      for (TaskTrackerStatus status : taskTrackers) {
        HttpHost host = new HttpHost(status.getHost(), status.getHttpPort());
        if (mesosTrackers.containsKey(host)) {
          mesosTrackers.get(host).active = true;
          idleMapSlots += status.getAvailableMapSlots();
          idleReduceSlots += status.getAvailableReduceSlots();
        }
      }

      // Consider the TaskTrackers that have yet to become active as being idle,
      // otherwise we will launch excessive TaskTrackers.
      int inactiveMapSlots = 0;
      int inactiveReduceSlots = 0;
      for (MesosTracker tracker : mesosTrackers.values()) {
        if (!tracker.active) {
          inactiveMapSlots += tracker.mapSlots;
          inactiveReduceSlots += tracker.reduceSlots;
        }
      }

      // Compute how many slots we need to allocate.
      int neededMapSlots = Math.max(0, pendingMaps - idleMapSlots);
      int neededReduceSlots = Math.max(0, pendingReduces - idleReduceSlots);

      LOG.info(join("\n", Arrays.asList(
          "JobTracker Status",
          "      Pending Map Tasks: " + pendingMaps,
          "   Pending Reduce Tasks: " + pendingReduces,
          "         Idle Map Slots: " + idleMapSlots,
          "      Idle Reduce Slots: " + idleReduceSlots,
          "     Inactive Map Slots: " + inactiveMapSlots
                                      + " (launched but no hearbeat yet)",
          "  Inactive Reduce Slots: " + inactiveReduceSlots
                                      + " (launched but no hearbeat yet)",
          "       Needed Map Slots: " + neededMapSlots,
          "    Needed Reduce Slots: " + neededReduceSlots)));

      // Launch TaskTrackers to satisfy the slot requirements.
      // TODO(bmahler): Consider slotting intelligently.
      // Ex: If more map slots are needed, but no reduce slots are needed,
      // launch a map-only TaskTracker to better satisfy the slot needs.
      for (Offer offer : offers) {
        if (neededMapSlots <= 0 && neededReduceSlots <= 0) {
          driver.declineOffer(offer.getId());
          continue;
        }

        double cpus = -1.0;
        double mem = -1.0;
        double disk = -1.0;
        Set<Integer> ports = new HashSet<Integer>(2);

        // Pull out the cpus, memory, disk, and 2 ports from the offer.
        for (Resource resource : offer.getResourcesList()) {
          if (resource.getName().equals("cpus")
              && resource.getType() == Value.Type.SCALAR) {
            cpus = resource.getScalar().getValue();
          } else if (resource.getName().equals("mem")
              && resource.getType() == Value.Type.SCALAR) {
            mem = resource.getScalar().getValue();
          } else if (resource.getName().equals("disk")
              && resource.getType() == Value.Type.SCALAR) {
            disk = resource.getScalar().getValue();
          } else if (resource.getName().equals("ports")
              && resource.getType() == Value.Type.RANGES) {
            for (Value.Range range : resource.getRanges().getRangeList()) {
              if (ports.size() < 2)
                ports.add((int) range.getBegin());
              if (ports.size() < 2)
                ports.add((int) range.getEnd());
            }
          }
        }

        int mapSlots = conf.getInt("mapred.tasktracker.map.tasks.maximum",
            MAP_SLOTS_DEFAULT);
        int reduceSlots = conf.getInt("mapred.tasktracker.reduce.tasks.maximum",
            REDUCE_SLOTS_DEFAULT);

        double slotCpus = conf.getFloat("mapred.mesos.slot.cpus",
            (float) SLOT_CPUS_DEFAULT);
        double slotDisk = conf.getInt("mapred.mesos.slot.disk",
            SLOT_DISK_DEFAULT);
        double slotMem = conf.getInt("mapred.mesos.slot.mem",
            SLOT_JVM_HEAP_DEFAULT + JVM_MEM_OVERHEAD);
        double slotJVMHeap = slotMem - JVM_MEM_OVERHEAD;

        // Total resource requirements for the container (TaskTracker + map/red
        // tasks).
        double containerCpus = (mapSlots + reduceSlots) * slotCpus
            + TASKTRACKER_CPUS;
        double containerMem = (mapSlots + reduceSlots) * slotMem
            + TASKTRACKER_MEM;
        double containerDisk = (mapSlots + reduceSlots) * slotDisk;

        if (containerCpus > cpus || containerMem > mem || containerDisk > disk
            || ports.size() < 2) {
          LOG.info(join("\n", Arrays.asList(
              "Declining offer with insufficient resources for a TaskTracker: ",
              "  cpus: offered " + cpus + " needed " + containerCpus,
              "  mem : offered " + mem + " needed " + containerMem,
              "  disk: offered " + disk + " needed " + containerDisk,
              "  ports: " + (ports.size() < 2
                            ? " less than 2 offered"
                            : " at least 2 (sufficient)"),
              offer.getResourcesList().toString())));

          driver.declineOffer(offer.getId());
          continue;
        }

        Integer[] portArray = ports.toArray(new Integer[2]);
        HttpHost httpAddress = new HttpHost(offer.getHostname(), portArray[0]);
        HttpHost reportAddress = new HttpHost(offer.getHostname(), portArray[1]);

        TaskID taskId = TaskID.newBuilder()
            .setValue("Task_Tracker_" + launchedTrackers++).build();

        LOG.info("Launching task " + taskId.getValue() + " on "
            + httpAddress.toString());

        // Add this tracker to Mesos tasks.
        mesosTrackers.put(httpAddress, new MesosTracker(httpAddress, taskId,
            mapSlots, reduceSlots));

        // Create the environment depending on whether the executor is going to be
        // run locally.
        // TODO(vinod): Do not pass the mapred config options as environment
        // variables.
        Protos.Environment.Builder envBuilder = Protos.Environment
            .newBuilder()
            .addVariables(
                Protos.Environment.Variable
                    .newBuilder()
                    .setName("mapred.job.tracker")
                    .setValue(jobTrackerAddress.getHostName() + ':'
                        + jobTrackerAddress.getPort()))
            .addVariables(
                Protos.Environment.Variable
                    .newBuilder()
                    .setName("mapred.task.tracker.http.address")
                    .setValue(
                        httpAddress.getHostName() + ':' + httpAddress.getPort()))
            .addVariables(
                Protos.Environment.Variable
                    .newBuilder()
                    .setName("mapred.task.tracker.report.address")
                    .setValue(reportAddress.getHostName() + ':'
                        + reportAddress.getPort()))
            .addVariables(
                Protos.Environment.Variable.newBuilder()
                    .setName("mapred.map.child.java.opts")
                    .setValue("-Xmx" + slotJVMHeap + "m"))
            .addVariables(
                Protos.Environment.Variable.newBuilder()
                    .setName("mapred.reduce.child.java.opts")
                    .setValue("-Xmx" + slotJVMHeap + "m"))
            .addVariables(
                Protos.Environment.Variable.newBuilder()
                    .setName("HADOOP_HEAPSIZE")
                    .setValue("" + TASKTRACKER_JVM_HEAP));

        // Set java specific environment, appropriately.
        Map<String, String> env = System.getenv();
        if (env.containsKey("JAVA_HOME")) {
          envBuilder.addVariables(Protos.Environment.Variable.newBuilder()
              .setName("JAVA_HOME")
              .setValue(env.get("JAVA_HOME")));
        }

        if (env.containsKey("JAVA_LIBRARY_PATH")) {
          envBuilder.addVariables(Protos.Environment.Variable.newBuilder()
              .setName("JAVA_LIBRARY_PATH")
              .setValue(env.get("JAVA_LIBRARY_PATH")));
        }

        // Command info differs when performing a local run.
        CommandInfo commandInfo = null;
        String master = conf.get("mapred.mesos.master", "local");

        if (master.equals("local")) {
          try {
            commandInfo = CommandInfo.newBuilder()
                .setEnvironment(envBuilder)
                .setValue(new File("bin/mesos-executor").getCanonicalPath())
                .build();
          } catch (IOException e) {
            LOG.fatal("Failed to find Mesos executor ", e);
            System.exit(1);
          }
        } else {
          String uri = conf.get("mapred.mesos.executor");
          commandInfo = CommandInfo.newBuilder()
              .setEnvironment(envBuilder)
              .setValue("cd hadoop-* && ./bin/mesos-executor")
              .addUris(CommandInfo.URI.newBuilder().setValue(uri)).build();
        }

        TaskInfo info = TaskInfo
            .newBuilder()
            .setName(taskId.getValue())
            .setTaskId(taskId)
            .setSlaveId(offer.getSlaveId())
            .addResources(
                Resource
                    .newBuilder()
                    .setName("cpus")
                    .setType(Value.Type.SCALAR)
                    .setScalar(Value.Scalar.newBuilder().setValue(
                        (mapSlots + reduceSlots) * slotCpus)))
            .addResources(
                Resource
                    .newBuilder()
                    .setName("mem")
                    .setType(Value.Type.SCALAR)
                    .setScalar(Value.Scalar.newBuilder().setValue(
                        (mapSlots + reduceSlots) * slotMem)))
            .addResources(
                Resource
                    .newBuilder()
                    .setName("disk")
                    .setType(Value.Type.SCALAR)
                    .setScalar(Value.Scalar.newBuilder().setValue(
                        (mapSlots + reduceSlots) * slotDisk)))
            .addResources(
                Resource
                    .newBuilder()
                    .setName("ports")
                    .setType(Value.Type.RANGES)
                    .setRanges(
                        Value.Ranges
                            .newBuilder()
                            .addRange(Value.Range.newBuilder()
                                .setBegin(httpAddress.getPort())
                                .setEnd(httpAddress.getPort()))
                            .addRange(Value.Range.newBuilder()
                                .setBegin(reportAddress.getPort())
                                .setEnd(reportAddress.getPort()))))
            .setExecutor(
                ExecutorInfo
                    .newBuilder()
                    .setExecutorId(ExecutorID.newBuilder().setValue(
                        "executor_" + taskId.getValue()))
                    .setName("Hadoop TaskTracker")
                    .setSource(taskId.getValue())
                    .addResources(
                        Resource
                            .newBuilder()
                            .setName("cpus")
                            .setType(Value.Type.SCALAR)
                            .setScalar(Value.Scalar.newBuilder().setValue(
                                (TASKTRACKER_CPUS))))
                    .addResources(
                        Resource
                            .newBuilder()
                            .setName("mem")
                            .setType(Value.Type.SCALAR)
                            .setScalar(Value.Scalar.newBuilder().setValue(
                                (TASKTRACKER_MEM)))).setCommand(commandInfo))
            .build();

        driver.launchTasks(offer.getId(), Arrays.asList(info));

        neededMapSlots -= mapSlots;
        neededReduceSlots -= reduceSlots;
      }

      if (neededMapSlots <= 0 && neededReduceSlots <= 0) {
        LOG.info("Satisfied map and reduce slots needed.");
      } else {
        LOG.info("Unable to fully satisfy needed map/reduce slots: "
            + (neededMapSlots > 0 ? neededMapSlots + " map slots " : "")
            + (neededReduceSlots > 0 ? neededReduceSlots + " reduce slots " : "")
            + "remaining");
      }
    }
  }

  @Override
  public synchronized void offerRescinded(SchedulerDriver schedulerDriver,
      OfferID offerID) {
    LOG.warn("Rescinded offer: " + offerID.getValue());
  }

  @Override
  public synchronized void statusUpdate(SchedulerDriver schedulerDriver,
      Protos.TaskStatus taskStatus) {
    LOG.info("Status update of " + taskStatus.getTaskId().getValue()
        + " to " + taskStatus.getState().name()
        + " with message " + taskStatus.getMessage());

    // Remove the TaskTracker if the corresponding Mesos task has reached a
    // terminal state.
    switch (taskStatus.getState()) {
      case TASK_FINISHED:
      case TASK_FAILED:
      case TASK_KILLED:
      case TASK_LOST:
        // Make a copy to iterate over keys and delete values.
        Set<HttpHost> trackers = new HashSet<HttpHost>(mesosTrackers.keySet());

        // Remove the task from the map.
        for (HttpHost tracker : trackers) {
          if (mesosTrackers.get(tracker).taskId.equals(taskStatus.getTaskId())) {
            LOG.info("Removing terminated TaskTracker: " + tracker);
            mesosTrackers.remove(tracker);
          }
        }
        break;
      case TASK_STAGING:
      case TASK_STARTING:
      case TASK_RUNNING:
        break;
      default:
        LOG.error("Unexpected TaskStatus: " + taskStatus.getState().name());
        break;
    }
  }

  @Override
  public synchronized void frameworkMessage(SchedulerDriver schedulerDriver,
      ExecutorID executorID, SlaveID slaveID, byte[] bytes) {
    LOG.info("Framework Message of " + bytes.length + " bytes"
        + " from executor " + executorID.getValue()
        + " on slave " + slaveID.getValue());
  }

  @Override
  public synchronized void disconnected(SchedulerDriver schedulerDriver) {
    LOG.warn("Disconnected from Mesos master.");
  }

  @Override
  public synchronized void slaveLost(SchedulerDriver schedulerDriver,
      SlaveID slaveID) {
    LOG.warn("Slave lost: " + slaveID.getValue());
  }

  @Override
  public synchronized void executorLost(SchedulerDriver schedulerDriver,
      ExecutorID executorID, SlaveID slaveID, int status) {
    LOG.warn("Executor " + executorID.getValue()
        + " lost with status " + status + " on slave " + slaveID);
  }

  @Override
  public synchronized void error(SchedulerDriver schedulerDriver, String s) {
    LOG.error("Error from scheduler driver: " + s);
  }

  /**
   * Used to track the our launched TaskTrackers.
   */
  private class MesosTracker {
    public HttpHost host;
    public TaskID taskId;
    public int mapSlots;
    public int reduceSlots;
    public boolean active = false; // Set once tracked by the JobTracker.

    // Tracks Hadoop tasks running on the tracker.
    public Set<TaskAttemptID> hadoopTasks = new HashSet<TaskAttemptID>();

    public MesosTracker(HttpHost host, TaskID taskId, int mapSlots,
        int reduceSlots) {
      this.host = host;
      this.taskId = taskId;
      this.mapSlots = mapSlots;
      this.reduceSlots = reduceSlots;
    }
  }
}
