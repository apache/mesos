package org.apache.hadoop.mapred;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.mapred.TaskStatus.State;
import org.apache.hadoop.mapred.TaskTrackerStatus;
import org.apache.hadoop.net.Node;

import mesos.ExecutorInfo;
import mesos.FrameworkMessage;
import mesos.Scheduler;
import mesos.SchedulerDriver;
import mesos.SlaveOffer;
import mesos.TaskDescription;
import mesos.TaskState;

public class FrameworkScheduler extends Scheduler {
  public static final Log LOG =
    LogFactory.getLog(FrameworkScheduler.class);
  public static final long KILL_UNLAUNCHED_TASKS_SLEEP_TIME = 2000;

  private static class MesosTask {
    final boolean isMap;
    final int mesosId;
    final String host;
    final long creationTime;
    
    TaskAttemptID hadoopId;
    
    MesosTask(boolean isMap, int mesosId, String host) {
      this.isMap = isMap;
      this.mesosId = mesosId;
      this.host = host;
      this.creationTime = System.currentTimeMillis();
    }

    boolean isAssigned() {
      return hadoopId != null;
    }
    
    void assign(Task task) {
      hadoopId = task.getTaskID();
    }
  }
  
  private static class TaskTrackerInfo {
    String mesosSlaveId;
    List<MesosTask> maps = new LinkedList<MesosTask>();
    List<MesosTask> reduces = new LinkedList<MesosTask>();
    
    public TaskTrackerInfo(String mesosSlaveId) {
      this.mesosSlaveId = mesosSlaveId;
    }
    
    void add(MesosTask nt) {
      if (nt.isMap)
        maps.add(nt);
      else
        reduces.add(nt);
    }

    public void remove(MesosTask nt) {
      if (nt.isMap)
        maps.remove(nt);
      else
        reduces.remove(nt);
    }
  }
  
  private class KillTimedOutTasksThread extends Thread {
    @Override
    public void run() {
      while (running) {
        killTimedOutTasks();
        try { Thread.sleep(KILL_UNLAUNCHED_TASKS_SLEEP_TIME); }
        catch (Exception e) {}
      }
    }
  }
  
  private MesosScheduler mesosSched;
  private SchedulerDriver driver;
  private String frameworkId;
  private Configuration conf;
  private JobTracker jobTracker;
  private boolean running;
  private AtomicInteger nextMesosTaskId = new AtomicInteger(0);
  
  private int cpusPerTask;
  private int memPerTask;
  private long localityWait;
  
  private Map<String, TaskTrackerInfo> ttInfos =
    new HashMap<String, TaskTrackerInfo>();
  
  private Map<TaskAttemptID, MesosTask> hadoopIdToMesosTask =
    new HashMap<TaskAttemptID, MesosTask>();
  private Map<Integer, MesosTask> mesosIdToMesosTask =
    new HashMap<Integer, MesosTask>();
  
  // Counts of various kinds of Mesos tasks
  // TODO: Figure out a better way to keep track of these
  int unassignedMaps = 0;
  int unassignedReduces = 0;
  int assignedMaps = 0;
  int assignedReduces = 0;
  
  // Variables used for delay scheduling
  boolean lastMapWasLocal = true;
  long timeWaitedForLocalMap = 0;
  long lastCanLaunchMapTime = -1;
  
  // Node slot couts
  // TODO: These should be configurable per node rather than fixed like this
  int maxMapsPerNode;
  int maxReducesPerNode;
  
  public FrameworkScheduler(MesosScheduler mesosSched) {
    this.mesosSched = mesosSched;
    this.conf = mesosSched.getConf();
    this.jobTracker = mesosSched.jobTracker;
    cpusPerTask = conf.getInt("mapred.mesos.task.cpus", 1);
    memPerTask = conf.getInt("mapred.mesos.task.mem", 1024);
    localityWait = conf.getLong("mapred.mesos.localitywait", 5000);
    maxMapsPerNode = conf.getInt("mapred.tasktracker.map.tasks.maximum", 2);
    maxReducesPerNode = conf.getInt("mapred.tasktracker.reduce.tasks.maximum", 2);
  }

  @Override
  public void registered(SchedulerDriver d, String fid) {
    this.driver = d;
    this.frameworkId = fid;
    LOG.info("Registered with Mesos, with framework ID " + fid);
    running = true;
    new KillTimedOutTasksThread().start();
  }
  
  public void cleanUp() {
    running = false;
  }
  
  @Override
  public void resourceOffer(SchedulerDriver d, String oid,
      List<SlaveOffer> offers) {
    try {
      synchronized(jobTracker) {
        LOG.info("Got resource offer " + oid);
        List<TaskDescription> tasks = new ArrayList<TaskDescription>();
        
        int numOffers = (int) offers.size();
        int[] cpus = new int[numOffers];
        int[] mem = new int[numOffers];

        // Count up the amount of free CPUs and memory on each node 
        for (int i = 0; i < numOffers; i++) {
          SlaveOffer offer = offers.get(i);
          cpus[i] = Integer.parseInt(offer.getParams().get("cpus"));
          mem[i] = Integer.parseInt(offer.getParams().get("mem"));
        }
        
        // Assign tasks to the nodes in a round-robin manner, and stop when we
        // are unable to assign a task to any node.
        // We do this by keeping a linked list of indices of nodes for which
        // we are still considering assigning tasks. Whenever we can't find a
        // new task for a node, we remove it from the list. When the list is
        // empty, no further assignments can be made. This algorithm was chosen
        // because it minimizing the amount of scanning we need to do if we
        // get a large set of offered nodes.
        List<Integer> indices = new LinkedList<Integer>();
        for (int i = 0; i < numOffers; i++) {
          indices.add(i);
        }
        while (indices.size() > 0) {
          for (Iterator<Integer> it = indices.iterator(); it.hasNext();) {
            int i = it.next();
            SlaveOffer offer = offers.get(i);
            TaskDescription task = findTask(
                offer.getSlaveId(), offer.getHost(), cpus[i], mem[i]);
            if (task != null) {
              cpus[i] -= Integer.parseInt(task.getParams().get("cpus"));
              mem[i] -= Integer.parseInt(task.getParams().get("mem"));
              tasks.add(task);
            } else {
              it.remove();
            }
          }
        }
        
        Map<String, String> params = new HashMap<String, String>();
        params.put("timeout", "1");
        d.replyToOffer(oid, tasks, params);
      }
    } catch(Exception e) {
      LOG.error("Error in resourceOffer", e);
    }
  }
  
  private TaskTrackerInfo getTaskTrackerInfo(String host, String slaveId) {
    if (ttInfos.containsKey(host)) {
      return ttInfos.get(host);
    } else {
      TaskTrackerInfo info = new TaskTrackerInfo(slaveId);
      ttInfos.put(host, info);
      return info;
    }
  }
  
  // Find a single task for a given node. Assumes JobTracker is locked.
  private TaskDescription findTask(
      String slaveId, String host, int cpus, int mem) {
    if (cpus < cpusPerTask || mem < memPerTask) {
      return null; // Too few resources are left on the node
    }
    
    TaskTrackerInfo ttInfo = getTaskTrackerInfo(host, slaveId);

    // Pick whether to launch a map or a reduce based on available tasks
    String taskType = null;
    boolean haveMaps = canLaunchMap(host);
    boolean haveReduces = canLaunchReduce(host);
    LOG.info("Looking at " + host + ": haveMaps=" + haveMaps + 
        ", haveReduces=" + haveReduces);
    if (!haveMaps && !haveReduces) {
      return null;
    } else if (haveMaps && !haveReduces) {
      taskType = "map";
    } else if (haveReduces && !haveMaps) {
      taskType = "reduce";
    } else {
      float mapToReduceRatio = 1;
      if (ttInfo.reduces.size() < ttInfo.maps.size() / mapToReduceRatio)
        taskType = "reduce";
      else
        taskType = "map";
    }
    LOG.info("Task type chosen: " + taskType);
    
    // Get a Mesos task ID for the new task
    int mesosId = newMesosTaskId();
    
    // Remember that it is launched
    boolean isMap = taskType.equals("map");
    if (isMap) {
      unassignedMaps++;
    } else {
      unassignedReduces++;
    }
    MesosTask nt = new MesosTask(isMap, mesosId, host);
    mesosIdToMesosTask.put(mesosId, nt);
    ttInfo.add(nt);
    
    // Create a task description to pass back to Mesos
    String name = "task " + mesosId + " (" + taskType + ")";
    Map<String, String> params = new HashMap<String, String>();
    params.put("cpus", "" + cpusPerTask);
    params.put("mem", "" + memPerTask);
    return new TaskDescription(mesosId, slaveId, name, params, new byte[0]);
  }

  private int newMesosTaskId() {
    return nextMesosTaskId.getAndIncrement();
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
  
  // TODO: Make this return a count instead of a boolean?
  // TODO: Cache result for some time so we don't iterate through all jobs
  // and tasks every time we get a resource offer?
  private boolean canLaunchMap(String host) {
    // Check whether the TT is saturated on maps
    TaskTrackerInfo ttInfo = ttInfos.get(host);
    if (ttInfo == null) {
      LOG.error("No TaskTrackerInfo for " + host + "! This shouldn't happen.");
      return false;
    }
    if (ttInfo.maps.size() >= maxMapsPerNode) {
      return false;
    }
    
    // Compute the total demand for maps to make sure we don't exceed it
    Collection<JobInProgress> jobs = jobTracker.jobs.values();
    int neededMaps = 0;
    for (JobInProgress job : jobs) {
      if (job.getStatus().getRunState() == JobStatus.RUNNING) {
        neededMaps += job.pendingMaps();
      }
    }
    // TODO (!!!): Count speculatable tasks and add them to neededMaps
    
    if (unassignedMaps < neededMaps) {
      // Figure out what locality level to allow using delay scheduling
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
      // Look for a map with the required level
      for (JobInProgress job: jobs) {
        int state = job.getStatus().getRunState();
        if (state == JobStatus.RUNNING && hasMapToLaunch(job, host, maxLevel)) {
          return true;
        }
      }
    }
    
    // If we didn't launch any tasks, but there are pending jobs in the queue,
    // ensure that at least one TaskTracker is running to execute setup tasks
    int numTrackers = jobTracker.getClusterStatus().getTaskTrackers();
    if (jobs.size() > 0 && numTrackers == 0 && totalMesosTasks() == 0) {
      LOG.info("Going to launch map task for setup / cleanup");
      return true;
    }
    
    return false;
  }
  
  private int totalMesosTasks() {
    return unassignedMaps + unassignedReduces + assignedMaps + assignedReduces;
  }

  // TODO: Make this return a count instead of a boolean?
  // TODO: Cache result for some time so we don't iterate through all jobs
  // and tasks every time we get a resource offer?
  private boolean canLaunchReduce(String host) {
    // Check whether the TT is saturated on reduces
    TaskTrackerInfo ttInfo = ttInfos.get(host);
    if (ttInfo == null) {
      LOG.error("No TaskTrackerInfo for " + host + "! This shouldn't happen.");
      return false;
    }
    if (ttInfo.reduces.size() >= maxReducesPerNode) {
      return false;
    }
    
    // Compute total demand for reduces, to make sure we don't exceed it
    Collection<JobInProgress> jobs = jobTracker.jobs.values();
    int neededReduces = 0;
    for (JobInProgress job : jobs) {
      if (job.getStatus().getRunState() == JobStatus.RUNNING) {
        neededReduces += job.pendingReduces();
      }
    }
    // TODO (!!!): Count speculatable tasks and add them to neededReduces
    
    if (neededReduces > unassignedReduces) {
      // Find a reduce to launch
      for (JobInProgress job: jobs) {
        int state = job.getStatus().getRunState();
        if (state == JobStatus.RUNNING && hasReduceToLaunch(job)) {
          return true;
        }
      }
    }
    
    return false;
  }
  
  @Override
  public void statusUpdate(SchedulerDriver d, mesos.TaskStatus status) {
    TaskState state = status.getState();
    if (state == TaskState.TASK_FINISHED || state == TaskState.TASK_FAILED ||
        state == TaskState.TASK_KILLED || state == TaskState.TASK_LOST) {
      synchronized (jobTracker) {
        int mesosId = status.getTaskId();
        MesosTask nt = mesosIdToMesosTask.get(mesosId);
        if (nt != null) {
          removeTask(nt);
        }
      }
    }
  }

  /**
   * Called by JobTracker to ask us to launch tasks on a heartbeat.
   * 
   * This is currently kind of silly; would be better to grab tasks when
   * we respond to the Mesos assignment, but then we'd need to be willing to
   * launch TaskTrackers everywhere
   */
  public List<Task> assignTasks(TaskTrackerStatus tts) {
    synchronized (jobTracker) {      
      try {
        Collection<JobInProgress> jobs = jobTracker.jobs.values();

        String host = tts.getHost();
        LOG.info("In FrameworkScheduler.assignTasks for " + host);
        
        TaskTrackerInfo ttInfo = ttInfos.get(host);
        if (ttInfo == null) {
          LOG.error("No TaskTrackerInfo for " + host + "! This shouldn't happen.");
          return null;
        }
        
        int clusterSize = jobTracker.getClusterStatus().getTaskTrackers();
        int numHosts = jobTracker.getNumberOfUniqueHosts();
        
        // Assigned tasks
        List<Task> assignedTasks = new ArrayList<Task>();
        
        // Identify unassigned maps and reduces on this TT
        List<MesosTask> assignableMaps = new ArrayList<MesosTask>();
        List<MesosTask> assignableReduces = new ArrayList<MesosTask>();
        for (MesosTask nt: ttInfo.maps)
          if (!nt.isAssigned())
            assignableMaps.add(nt);
        for (MesosTask nt: ttInfo.reduces)
          if (!nt.isAssigned())
            assignableReduces.add(nt);
        
        // Get some iterators for the unassigned tasks
        Iterator<MesosTask> mapIter = assignableMaps.iterator();
        Iterator<MesosTask> reduceIter = assignableReduces.iterator();
        
        // Go through jobs in FIFO order and look for tasks to launch
        for (JobInProgress job: jobs) {
          if (job.getStatus().getRunState() == JobStatus.RUNNING) {
            // If the node has unassigned maps, try to launch map tasks
            while (mapIter.hasNext()) {
              Task task = job.obtainNewMapTask(tts, clusterSize, numHosts);
              if (task != null) {
                MesosTask nt = mapIter.next();
                nt.assign(task);
                unassignedMaps--;
                assignedMaps++;
                hadoopIdToMesosTask.put(task.getTaskID(), nt);
                assignedTasks.add(task);
                task.extraData = "" + nt.mesosId;
              } else {
                break;
              }
            }
            // If the node has unassigned reduces, try to launch reduce tasks
            while (reduceIter.hasNext()) {
              Task task = job.obtainNewReduceTask(tts, clusterSize, numHosts);
              if (task != null) {
                MesosTask nt = reduceIter.next();
                nt.assign(task);
                unassignedReduces--;
                assignedReduces++;
                hadoopIdToMesosTask.put(task.getTaskID(), nt);
                assignedTasks.add(task);
                task.extraData = "" + nt.mesosId;
              } else {
                break;
              }
            }
          }
        }
        
        return assignedTasks;
      } catch (IOException e) {
        LOG.error("IOException in assignTasks", e);
        return null;
      }
    }
  }

  private void removeTask(MesosTask nt) {
    synchronized (jobTracker) {
      mesosIdToMesosTask.remove(nt.mesosId);
      if (nt.hadoopId != null) {
        hadoopIdToMesosTask.remove(nt.hadoopId);
      }
      TaskTrackerInfo ttInfo = ttInfos.get(nt.host);
      if (ttInfo != null) {
        ttInfo.remove(nt);
      }
      if (nt.isMap) {
        if (nt.isAssigned())
          assignedMaps--;
        else
          unassignedMaps--;
      } else {
        if (nt.isAssigned())
          assignedReduces--;
        else
          unassignedReduces--;
      }
    }
  }

  private void askExecutorToUpdateStatus(MesosTask nt, TaskState state) {
    TaskTrackerInfo ttInfo = ttInfos.get(nt.host);
    if (ttInfo != null) {
      HadoopFrameworkMessage message = new HadoopFrameworkMessage(
          HadoopFrameworkMessage.Type.S2E_SEND_STATUS_UPDATE, state.toString());
      try {
        LOG.info("Asking slave " + ttInfo.mesosSlaveId + " to update status");
        driver.sendFrameworkMessage(new FrameworkMessage(
            ttInfo.mesosSlaveId, nt.mesosId, message.serialize()));
      } catch (IOException e) {
        // This exception would only get thrown if we couldn't serialize the
        // HadoopFrameworkMessage, which is a serious problem; crash the JT
        LOG.fatal("Failed to serialize HadoopFrameworkMessage", e);
        throw new RuntimeException(
            "Failed to serialize HadoopFrameworkMessage", e);
      }
    }
  }

  // Kill any unlaunched tasks that have timed out
  public void killTimedOutTasks() {
    synchronized (jobTracker) {
      long curTime = System.currentTimeMillis();
      long timeout = 2 * jobTracker.getNextHeartbeatInterval();
      for (TaskTrackerInfo tt: ttInfos.values()) {
        killTimedOutTasks(tt.maps, curTime - timeout);
        killTimedOutTasks(tt.reduces, curTime - timeout);
      }
    }
  }
    
  private void killTimedOutTasks(List<MesosTask> tasks, long minCreationTime) {
    List<MesosTask> toRemove = new ArrayList<MesosTask>();
    for (MesosTask nt: tasks) {
      if (!nt.isAssigned() && nt.creationTime < minCreationTime) {
        toRemove.add(nt);
      }
    }
    for (MesosTask nt: toRemove) {
      askExecutorToUpdateStatus(nt, TaskState.TASK_KILLED);
      removeTask(nt);
    }
  }
  
  @Override
  public void frameworkMessage(SchedulerDriver d, FrameworkMessage message) {
    // TODO: Respond to E2S_KILL_REQUEST message by killing a task
  }
  
  // Methods to check whether a job has runnable tasks
  
  /**
   * Check whether the job can launch a map task on a given node, with a given
   * level of locality (maximum cache level). Also includes job setup and
   * cleanup tasks, as well as map cleanup tasks.
   * 
   * This is currently fairly long because it replicates a lot of the logic
   * in findNewMapTask. Unfortunately, it's not easy to just use findNewMapTask
   * directly, because that requires a TaskTracker. One way to avoid requiring
   * this method would be to just launch TaskTrackers on every node, without
   * first checking for locality.
   */
  boolean hasMapToLaunch(JobInProgress job, String host, int maxCacheLevel) {
    synchronized (job) {
      // For scheduling a map task, we have two caches and a list (optional)
      //  I)   one for non-running task
      //  II)  one for running task (this is for handling speculation)
      //  III) a list of TIPs that have empty locations (e.g., dummy splits),
      //       the list is empty if all TIPs have associated locations
  
      // First a look up is done on the non-running cache and on a miss, a look 
      // up is done on the running cache. The order for lookup within the cache:
      //   1. from local node to root [bottom up]
      //   2. breadth wise for all the parent nodes at max level
  
      //if (canLaunchJobCleanupTask()) return true;
      //if (canLaunchSetupTask()) return true;
      if (!job.mapCleanupTasks.isEmpty()) return true;
      
      // Return false right away if the task cache isn't ready, either because
      // we are still initializing or because we are cleaning up
      if (job.nonRunningMapCache == null) return false;
      
      // We fall to linear scan of the list (III above) if we have misses in the 
      // above caches
  
      Node node = jobTracker.getNode(host);

      int maxLevel = job.getMaxCacheLevel();
      
      //
      // I) Non-running TIP :
      // 
  
      // 1. check from local node to the root [bottom up cache lookup]
      //    i.e if the cache is available and the host has been resolved
      //    (node!=null)
      if (node != null) {
        Node key = node;
        int level = 0;
        // maxCacheLevel might be greater than this.maxLevel if findNewMapTask is
        // called to schedule any task (local, rack-local, off-switch or speculative)
        // tasks or it might be NON_LOCAL_CACHE_LEVEL (i.e. -1) if findNewMapTask is
        //  (i.e. -1) if findNewMapTask is to only schedule off-switch/speculative
        // tasks
        int maxLevelToSchedule = Math.min(maxCacheLevel, maxLevel);
        for (level = 0;level < maxLevelToSchedule; ++level) {
          List <TaskInProgress> cacheForLevel = job.nonRunningMapCache.get(key);
          if (hasUnlaunchedTask(cacheForLevel)) {
            return true;
          }
          key = key.getParent();
        }
        
        // Check if we need to only schedule a local task (node-local/rack-local)
        if (level == maxCacheLevel) {
          return false;
        }
      }
  
      //2. Search breadth-wise across parents at max level for non-running 
      //   TIP if
      //     - cache exists and there is a cache miss 
      //     - node information for the tracker is missing (tracker's topology
      //       info not obtained yet)
  
      // collection of node at max level in the cache structure
      Collection<Node> nodesAtMaxLevel = jobTracker.getNodesAtMaxLevel();
  
      // get the node parent at max level
      Node nodeParentAtMaxLevel = 
        (node == null) ? null : JobTracker.getParentNode(node, maxLevel - 1);
      
      for (Node parent : nodesAtMaxLevel) {
  
        // skip the parent that has already been scanned
        if (parent == nodeParentAtMaxLevel) {
          continue;
        }
  
        List<TaskInProgress> cache = job.nonRunningMapCache.get(parent);
        if (hasUnlaunchedTask(cache)) {
          return true;
        }
      }
  
      // 3. Search non-local tips for a new task
      if (hasUnlaunchedTask(job.nonLocalMaps))
        return true;
      
      //
      // II) Running TIP :
      // 
   
      if (job.getMapSpeculativeExecution()) {
        long time = System.currentTimeMillis();
        float avgProg = job.status.mapProgress();
  
        // 1. Check bottom up for speculative tasks from the running cache
        if (node != null) {
          Node key = node;
          for (int level = 0; level < maxLevel; ++level) {
            Set<TaskInProgress> cacheForLevel = job.runningMapCache.get(key);
            if (cacheForLevel != null) {
              for (TaskInProgress tip: cacheForLevel) {
                if (tip.isRunning() && tip.hasSpeculativeTask(time, avgProg)) {
                  return true;
                }
              }
            }
            key = key.getParent();
          }
        }
  
        // 2. Check breadth-wise for speculative tasks
        
        for (Node parent : nodesAtMaxLevel) {
          // ignore the parent which is already scanned
          if (parent == nodeParentAtMaxLevel) {
            continue;
          }
  
          Set<TaskInProgress> cache = job.runningMapCache.get(parent);
          if (cache != null) {
            for (TaskInProgress tip: cache) {
              if (tip.isRunning() && tip.hasSpeculativeTask(time, avgProg)) {
                return true;
              }
            }
          }
        }
  
        // 3. Check non-local tips for speculation
        for (TaskInProgress tip: job.nonLocalRunningMaps) {
          if (tip.isRunning() && tip.hasSpeculativeTask(time, avgProg)) {
            return true;
          }
        }
      }
      
      return false;
    }
  }
  
  /**
   * Check whether a task list (from the non-running map cache) contains any
   * unlaunched tasks.
   */
  boolean hasUnlaunchedTask(Collection<TaskInProgress> cache) {
    if (cache != null)
      for (TaskInProgress tip: cache)
        if (tip.isRunnable() && !tip.isRunning())
          return true;
    return false;
  }
  
  /**
   * Check whether a job can launch a reduce task. Also includes reduce
   * cleanup tasks.
   * 
   * As with hasMapToLaunch, this duplicates the logic inside
   * findNewReduceTask. Please see the comment there for an explanation.
   */
  boolean hasReduceToLaunch(JobInProgress job) {
    synchronized (job) {
      // Return false if not enough maps have finished to launch reduces
      if (!job.scheduleReduces()) return false;
      
      // Check for a reduce cleanup task
      if (!job.reduceCleanupTasks.isEmpty()) return true;
      
      // Return false right away if the task cache isn't ready, either because
      // we are still initializing or because we are cleaning up
      if (job.nonRunningReduces == null) return false;
      
      // Check for an unlaunched reduce
      if (job.nonRunningReduces.size() > 0) return true;
      
      // Check for a reduce to be speculated
      if (job.getReduceSpeculativeExecution()) {
        long time = System.currentTimeMillis();
        float avgProg = job.status.reduceProgress();
        for (TaskInProgress tip: job.runningReduces) {
          if (tip.isRunning() && tip.hasSpeculativeTask(time, avgProg)) {
            return true;
          }
        }
      }
      
      return false;
    }
  }
}
