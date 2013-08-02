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
import java.util.Timer;
import java.util.TimerTask;
import java.util.Iterator;

import org.apache.commons.httpclient.HttpHost;
import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.mapreduce.server.jobtracker.TaskTracker;
import org.apache.hadoop.mapreduce.TaskType;
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
  private static final double JVM_MEM_OVERHEAD_PERCENT_DEFAULT = 0.1; // 10%.

  // TODO(vinod): Consider parsing the slot memory from the configuration jvm
  // heap options (e.g: mapred.child.java.opts).

  // NOTE: It appears that there's no real resource requirements for a
  // map / reduce slot. We therefore define a default slot as:
  // 0.2 cores.
  // 512 MB memory.
  // 1 GB of disk space.
  private static final double SLOT_CPUS_DEFAULT = 0.2; // 0.2 cores.
  private static final int SLOT_DISK_DEFAULT = 1024; // 1 GB.
  private static final int SLOT_JVM_HEAP_DEFAULT = 256; // 256MB.

  private static final double TASKTRACKER_CPUS = 1.0; // 1 core.
  private static final int TASKTRACKER_MEM_DEFAULT = 1024; // 1 GB.

  // The default behavior in Hadoop is to use 4 slots per TaskTracker:
  private static final int MAP_SLOTS_DEFAULT = 2;
  private static final int REDUCE_SLOTS_DEFAULT = 2;

  // The amount of time to wait for task trackers to launch before
  // giving up.
  private static final long LAUNCH_TIMEOUT_MS = 300000; // 5 minutes

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
        if (!job.isComplete()) {
          return;
        }

        LOG.info("Completed job : " + job.getJobID());

        List<TaskInProgress> completed = new ArrayList<TaskInProgress>();

        // Map tasks.
        completed.addAll(job.reportTasksInProgress(true, true));

        // Reduce tasks.
        completed.addAll(job.reportTasksInProgress(false, true));

        for (TaskInProgress task : completed) {
          // Check that this task actually belongs to this job
          if (task.getJob().getJobID() != job.getJobID()) {
            continue;
          }

          for (TaskStatus status : task.getTaskStatuses()) {
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

              LOG.info("Removing completed task : " + status.getTaskID()
                  + " of tracker " + status.getTaskTracker());

              mesosTracker.hadoopJobs.remove(job.getJobID());

              // If the TaskTracker doesn't have any running tasks, kill it.
              if (mesosTracker.hadoopJobs.isEmpty()) {
                LOG.info("Killing Mesos task: " + mesosTracker.taskId + " on host "
                    + mesosTracker.host);

                driver.killTask(mesosTracker.taskId);
		mesosTracker.timer.cancel();
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
        .setCheckpoint(conf.getBoolean("mapred.mesos.checkpoint", false))
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
        mesosTrackers.get(tracker).hadoopJobs.add(task.getJobID());
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

  public synchronized void killTracker(MesosTracker tracker) {
    driver.killTask(tracker.taskId);
    mesosTrackers.remove(tracker.host);
  }

  // For some reason, pendingMaps() and pendingReduces() doesn't return the
  // values we expect. We observed negative values, which may be related to
  // https://issues.apache.org/jira/browse/MAPREDUCE-1238. Below is the
  // algorithm that is used to calculate the pending tasks within the Hadoop
  // JobTracker sources (see 'printTaskSummary' in
  // src/org/apache/hadoop/mapred/jobdetails_jsp.java).
  private int getPendingTasks(TaskInProgress[] tasks) {
    int totalTasks = tasks.length;
    int runningTasks = 0;
    int finishedTasks = 0;
    int killedTasks = 0;
    for (int i = 0; i < totalTasks; ++i) {
      TaskInProgress task = tasks[i];
      if (task == null) {
        continue;
      }
      if (task.isComplete()) {
        finishedTasks += 1;
      } else if (task.isRunning()) {
        runningTasks += 1;
      } else if (task.wasKilled()) {
        killedTasks += 1;
      }
    }
    int pendingTasks = totalTasks - runningTasks - killedTasks - finishedTasks;
    return pendingTasks;
  }

  // This method uses explicit synchronization in order to avoid deadlocks when
  // accessing the JobTracker.
  @Override
  public void resourceOffers(SchedulerDriver schedulerDriver,
      List<Offer> offers) {
    // Before synchronizing, we pull all needed information from the JobTracker.
    final HttpHost jobTrackerAddress =
      new HttpHost(jobTracker.getHostname(), jobTracker.getTrackerPort());

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
        // JobStatus.pendingMaps/Reduces may return the wrong value on
        // occasion.  This seems to be safer.
        pendingMaps += getPendingTasks(progress.getTasks(TaskType.MAP));
        pendingReduces += getPendingTasks(progress.getTasks(TaskType.REDUCE));
      }

      // Mark active (heartbeated) TaskTrackers and compute idle slots.
      int idleMapSlots = 0;
      int idleReduceSlots = 0;
      int unhealthyTrackers = 0;

      for (TaskTrackerStatus status : taskTrackers) {
        if (!status.getHealthStatus().isNodeHealthy()) {
          // Skip this node if it's unhealthy.
          ++unhealthyTrackers;
          continue;
        }

        HttpHost host = new HttpHost(status.getHost(), status.getHttpPort());
        if (mesosTrackers.containsKey(host)) {
          mesosTrackers.get(host).active = true;
          mesosTrackers.get(host).timer.cancel();
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

      // To ensure Hadoop jobs begin promptly, we can specify a minimum number
      // of 'hot slots' to be available for use.  This addresses the
      // TaskTracker spin up delay that exists with Hadoop on Mesos.  This can
      // be a nuisance with lower latency applications, such as ad-hoc Hive
      // queries.
      int minimumMapSlots = conf.getInt("mapred.mesos.total.map.slots.minimum", 0);
      int minimumReduceSlots =
        conf.getInt("mapred.mesos.total.reduce.slots.minimum", 0);

      // Compute how many slots we need to allocate.
      int neededMapSlots = Math.max(
          minimumMapSlots - (idleMapSlots + inactiveMapSlots),
          pendingMaps - (idleMapSlots + inactiveMapSlots));
      int neededReduceSlots = Math.max(
          minimumReduceSlots  - (idleReduceSlots + inactiveReduceSlots),
          pendingReduces - (idleReduceSlots + inactiveReduceSlots));

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
              "    Needed Reduce Slots: " + neededReduceSlots,
              "     Unhealthy Trackers: " + unhealthyTrackers)));

      // Launch TaskTrackers to satisfy the slot requirements.
      // TODO(bmahler): Consider slotting intelligently.
      // Ex: If more map slots are needed, but no reduce slots are needed,
      // launch a map-only TaskTracker to better satisfy the slot needs.
      for (Offer offer : offers) {
        if (neededMapSlots <= 0 && neededReduceSlots <= 0) {
          driver.declineOffer(offer.getId());
          continue;
        }

        // Ensure these values aren't < 0.
        neededMapSlots = Math.max(0, neededMapSlots);
        neededReduceSlots = Math.max(0, neededReduceSlots);

        double cpus = -1.0;
        double mem = -1.0;
        double disk = -1.0;
        Set<Integer> ports = new HashSet<Integer>();

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
              Integer begin = (int)range.getBegin();
              Integer end = (int)range.getEnd();
              if (end < begin) {
                LOG.warn("Ignoring invalid port range: begin=" + begin + " end=" + end);
                continue;
              }
              while (begin <= end && ports.size() < 2) {
                ports.add(begin);
                begin += 1;
              }
            }
          }
        }

        int mapSlotsMax = conf.getInt("mapred.tasktracker.map.tasks.maximum",
            MAP_SLOTS_DEFAULT);
        int reduceSlotsMax =
          conf.getInt("mapred.tasktracker.reduce.tasks.maximum",
              REDUCE_SLOTS_DEFAULT);

        // What's the minimum number of map and reduce slots we should try to
        // launch?
        long mapSlots = 0;
        long reduceSlots = 0;

        double slotCpus = conf.getFloat("mapred.mesos.slot.cpus",
            (float) SLOT_CPUS_DEFAULT);
        double slotDisk = conf.getInt("mapred.mesos.slot.disk",
            SLOT_DISK_DEFAULT);

        int slotMem = conf.getInt("mapred.mesos.slot.mem",
            SLOT_JVM_HEAP_DEFAULT);
        long slotJVMHeap = Math.round((double)slotMem -
            (JVM_MEM_OVERHEAD_PERCENT_DEFAULT * slotMem));

        int tasktrackerMem = conf.getInt("mapred.mesos.tasktracker.mem",
              TASKTRACKER_MEM_DEFAULT);
        long tasktrackerJVMHeap = Math.round((double)tasktrackerMem -
            (JVM_MEM_OVERHEAD_PERCENT_DEFAULT * tasktrackerMem));

        // Minimum resource requirements for the container (TaskTracker + map/red
        // tasks).
        double containerCpus = TASKTRACKER_CPUS;
        double containerMem = tasktrackerMem;
        double containerDisk = 0;

        // Determine how many slots we can allocate.
        int slots = mapSlotsMax + reduceSlotsMax;
        slots = (int)Math.min(slots, (cpus - containerCpus) / slotCpus);
        slots = (int)Math.min(slots, (mem - containerMem) / slotMem);
        slots = (int)Math.min(slots, (disk - containerDisk) / slotDisk);

        // Is this offer too small for even the minimum slots?
        if (slots < 1 || ports.size() < 2) {
          LOG.info(join("\n", Arrays.asList(
                  "Declining offer with insufficient resources for a TaskTracker: ",
                  "  cpus: offered " + cpus + " needed " + containerCpus,
                  "  mem : offered " + mem + " needed " + containerMem,
                  "  disk: offered " + disk + " needed " + containerDisk,
                  "  ports: " + (ports.size() < 2
                    ? " less than 2 offered"
                    : " at least 2 (sufficient)"))));

          driver.declineOffer(offer.getId());
          continue;
        }

        // Is the number of slots we need sufficiently small? If so, we can
        // allocate exactly the number we need.
        if (slots >= neededMapSlots + neededReduceSlots && neededMapSlots <
            mapSlotsMax && neededReduceSlots < reduceSlotsMax) {
          mapSlots = neededMapSlots;
          reduceSlots = neededReduceSlots;
        } else {
          // Allocate slots fairly for this resource offer.
          double mapFactor = (double)neededMapSlots / (neededMapSlots + neededReduceSlots);
          double reduceFactor = (double)neededReduceSlots / (neededMapSlots + neededReduceSlots);
          // To avoid map/reduce slot starvation, don't allow more than 50%
          // spread between map/reduce slots when we need both mappers and
          // reducers.
          if (neededMapSlots > 0 && neededReduceSlots > 0) {
            if (mapFactor < 0.25) {
              mapFactor = 0.25;
            } else if (mapFactor > 0.75) {
              mapFactor = 0.75;
            }
            if (reduceFactor < 0.25) {
              reduceFactor = 0.25;
            } else if (reduceFactor > 0.75) {
              reduceFactor = 0.75;
            }
          }
          mapSlots = Math.min(Math.min((long)(mapFactor * slots), mapSlotsMax), neededMapSlots);
          // The remaining slots are allocated for reduces.
          slots -= mapSlots;
          reduceSlots = Math.min(Math.min(slots, reduceSlotsMax), neededReduceSlots);
        }

        Iterator<Integer> portIter = ports.iterator();
        HttpHost httpAddress = new HttpHost(offer.getHostname(), portIter.next());
        HttpHost reportAddress = new HttpHost(offer.getHostname(), portIter.next());

          // Check that this tracker is not already launched.  This problem was
          // observed on a few occasions, but not reliably.  The main symptom was
          // that entries in `mesosTrackers` were being lost, and task trackers
          // would be 'lost' mysteriously (probably because the ports were in
          // use).  This problem has since gone away with a rewrite of the port
          // selection code, but the check + logging is left here.
          // TODO(brenden): Diagnose this to determine root cause.

        if (mesosTrackers.containsKey(httpAddress)) {
          LOG.info(join("\n", Arrays.asList(
                  "Declining offer because host/port combination is in use: ",
                  "  cpus: offered " + cpus + " needed " + containerCpus,
                  "  mem : offered " + mem + " needed " + containerMem,
                  "  disk: offered " + disk + " needed " + containerDisk,
                  "  ports: " + ports)));

          driver.declineOffer(offer.getId());
          continue;
        }

        TaskID taskId = TaskID.newBuilder()
          .setValue("Task_Tracker_" + launchedTrackers++).build();

        LOG.info("Launching task " + taskId.getValue() + " on "
            + httpAddress.toString() + " with mapSlots=" + mapSlots + " reduceSlots=" + reduceSlots);

        // Add this tracker to Mesos tasks.
        mesosTrackers.put(httpAddress, new MesosTracker(httpAddress, taskId,
              mapSlots, reduceSlots, this));

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
              .setValue("" + tasktrackerJVMHeap))
          .addVariables(
              Protos.Environment.Variable.newBuilder()
              .setName("mapred.tasktracker.map.tasks.maximum")
              .setValue("" + mapSlots))
          .addVariables(
              Protos.Environment.Variable.newBuilder()
              .setName("mapred.tasktracker.reduce.tasks.maximum")
              .setValue("" + reduceSlots));

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
          commandInfo = CommandInfo.newBuilder()
            .setEnvironment(envBuilder)
            .setValue(new File("bin/hadoop").getCanonicalPath() +
                      " org.apache.hadoop.mapred.MesosExecutor")
            .build();
        } else {
          String uri = conf.get("mapred.mesos.executor");
          commandInfo = CommandInfo.newBuilder()
            .setEnvironment(envBuilder)
            .setValue("cd hadoop-* && " +
                      "./bin/hadoop org.apache.hadoop.mapred.MesosExecutor")
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
                    (tasktrackerMem)))).setCommand(commandInfo))
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
	    mesosTrackers.get(tracker).timer.cancel();
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
    public volatile HttpHost host;
    public TaskID taskId;
    public long mapSlots;
    public long reduceSlots;
    public volatile boolean active = false; // Set once tracked by the JobTracker.
    public Timer timer;
    public volatile MesosScheduler scheduler;

    // Tracks Hadoop job tasks running on the tracker.
    public Set<JobID> hadoopJobs = new HashSet<JobID>();

    public MesosTracker(HttpHost host, TaskID taskId, long mapSlots,
        long reduceSlots, MesosScheduler scheduler) {
      this.host = host;
      this.taskId = taskId;
      this.mapSlots = mapSlots;
      this.reduceSlots = reduceSlots;
      this.scheduler = scheduler;

      this.timer = new Timer();
      timer.schedule(new TimerTask() {
        @Override
        public void run() {
          synchronized (MesosTracker.this.scheduler) {
            // If the tracker activated while we were awaiting to acquire the
            // lock, return.
            if (MesosTracker.this.active) return;

            LOG.warn("Tracker " + MesosTracker.this.host + " failed to launch within " +
              LAUNCH_TIMEOUT_MS / 1000 + " seconds, killing it");
            MesosTracker.this.scheduler.killTracker(MesosTracker.this);
          }
        }
      }, LAUNCH_TIMEOUT_MS);
    }
  }
}
