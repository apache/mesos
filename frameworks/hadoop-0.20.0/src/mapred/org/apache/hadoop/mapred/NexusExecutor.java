package org.apache.hadoop.mapred;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hadoop.io.ObjectWritable;
import org.apache.hadoop.mapred.TaskStatus.State;

import nexus.Executor;
import nexus.ExecutorArgs;
import nexus.ExecutorDriver;
import nexus.FrameworkMessage;
import nexus.NexusExecutorDriver;
import nexus.TaskDescription;
import nexus.TaskState;

public class NexusExecutor extends Executor 
    implements InterTrackerProtocol {
  public static final Log LOG =
    LogFactory.getLog(NexusExecutor.class);

  private JobConf conf;
  private TaskTracker taskTracker;

  private ExecutorDriver driver;
  private String slaveId;
  
  private AtomicInteger nextRpcId = new AtomicInteger();
  
  private Map<Integer, RpcResponse> rpcResponses =
      new ConcurrentHashMap<Integer, RpcResponse>();
  
  private List<UnmappedTask> unmappedMaps = new LinkedList<UnmappedTask>();
  private List<UnmappedTask> unmappedReduces = new LinkedList<UnmappedTask>();
  private Map<TaskAttemptID, Integer> hadoopIdToNexusId =
      new HashMap<TaskAttemptID, Integer>();
  private Map<Integer, TaskAttemptID> nexusIdToHadoopId =
      new HashMap<Integer, TaskAttemptID>();
  private State state;

  /**
   * Represents a Nexus task that was launched but has not yet been mapped
   * to a Hadoop task. That is, we have acquired a slot for this task but
   * have not yet assigned it a map or reduce. We keep such tasks around
   * for a limited time (mapred.nexus.unmapped.task.lifetime) and then give
   * them up because they can arise from "mistakes" in accepting slot offers
   * (e.g. we thought we needed a new slot but then we didn't). To ensure
   * high utilization, we don't want to waste Nexus slots on unmapped tasks.
   */
  private static class UnmappedTask {
    int taskId;
    long creationTime;
    
    UnmappedTask(int taskId, long creationTime) {
      super();
      this.taskId = taskId;
      this.creationTime = creationTime;
    }
  }
  
  @Override
  public void init(ExecutorDriver driver, ExecutorArgs args) {
    try {
      this.driver = driver;
      slaveId = args.getSlaveId();
      conf = new JobConf();
      conf.set("mapred.job.tracker", new String(args.getData()));
      taskTracker = new TaskTracker(conf, this);
      new Thread("TaskTracker run thread") {
        @Override
        public void run() {
          taskTracker.run();
        }
      }.start();
    } catch (IOException e) {
      e.printStackTrace();
      System.exit(1);
    }
  }

  /**
   * Remove expired unmapped tasks. We call this right before sending out every
   * heartbeat to ensure that we never kill an unmapped task that we'll
   * eventually get a launch task action for from a previous heartbeat.
   * Note that the tracker's slot counts, which we may decrement here, will
   * be set in the heartbeat response by the implementation of heartbeat()
   * after it calls expireUnmappedTasks().
   */
  private void expireUnmappedTasks() {
    List<nexus.TaskStatus> updates = new ArrayList<nexus.TaskStatus>();
    synchronized (this) {
      long time = System.currentTimeMillis();
      long unmappedTaskLifetime = 2 * taskTracker.heartbeatInterval;
      List<List<UnmappedTask>> lists = new ArrayList<List<UnmappedTask>>();
      lists.add(unmappedMaps);
      lists.add(unmappedReduces);
      for (List<UnmappedTask> list: lists) {
        for (Iterator<UnmappedTask> it = list.iterator(); it.hasNext();) {
          UnmappedTask task = it.next();
          int taskId = task.taskId;
          if (time - task.creationTime > unmappedTaskLifetime) {
            // Remove the task and report it to Nexus as LOST
            LOG.info("Removing expired unmapped task " + taskId);
            it.remove();
            if (list == unmappedMaps) {
              taskTracker.mapSlots--;
            } else {
              taskTracker.reduceSlots--;
            }
            updates.add(
              new nexus.TaskStatus(taskId, TaskState.TASK_LOST, new byte[0]));
          }
        }
      }
    }
    for (nexus.TaskStatus update: updates) {
      driver.sendStatusUpdate(update);
    }
  }

  @Override
  public void killTask(ExecutorDriver d, int taskId) {
    synchronized (this) {
      TaskAttemptID hadoopId = nexusIdToHadoopId.get(taskId);
      taskTracker.killTask(hadoopId);
    }
  }

  @Override
  public void launchTask(ExecutorDriver d, TaskDescription taskDesc) {
    String taskType = new String(taskDesc.getArg());
    LOG.info("start_task " + taskDesc.getTaskId() + ": " + taskType);
    if (taskType.equals("map")) {
      synchronized (this) {
        unmappedMaps.add(new UnmappedTask(
            taskDesc.getTaskId(), System.currentTimeMillis()));
      }
      taskTracker.mapSlots++;
    } else {
      synchronized (this) {
        unmappedReduces.add(new UnmappedTask(
            taskDesc.getTaskId(), System.currentTimeMillis()));
      }
      taskTracker.reduceSlots++;
    }
  }
  
  @Override
  public void frameworkMessage(ExecutorDriver d, FrameworkMessage message) {
    try {
      //LOG.info("Got RPC response of size " + message.getData().length);
      ObjectWritable writable = new ObjectWritable();
      writable.setConf(conf);
      DataInputStream in = new DataInputStream(
          new ByteArrayInputStream(message.getData()));
      int rpcId = in.readInt();
      //LOG.info("Executor on " + slaveId + " got RPC response for ID " + 
      //         rpcId + " with length " + message.getData().length);
      writable.readFields(in);
      RpcResponse response = rpcResponses.get(rpcId);
      synchronized(response) {
        response.result = writable.get();
        response.ready = true;
        response.notify();
      }
    } catch (Exception e) {
      e.printStackTrace();
      System.exit(1);
    }
  }

  private static class RpcResponse {
    int rpcId;
    boolean ready = false;
    Object result;
    
    public RpcResponse(int rpcId) {
      this.rpcId = rpcId;
    }
  }

  @Override
  public String getBuildVersion() throws IOException {
    return (String) invokeRPC("getBuildVersion");
  }

  private Object invokeRPC(String method, Object... args) throws IOException {
    // Get a unique RPC ID for this call
    int rpcId = nextRpcId.getAndIncrement();
    //LOG.info("Executor on " + slaveId + " making RPC: " + method + 
    //         " with ID " + rpcId);
    
    // Create an RpcResponse for this call
    RpcResponse response = new RpcResponse(rpcId);
    rpcResponses.put(rpcId, response);
    
    // Send the call as a framework message
    ByteArrayOutputStream bos = new ByteArrayOutputStream();
    DataOutputStream out = new DataOutputStream(bos);
    out.writeInt(rpcId);
    out.writeUTF(method);
    out.writeInt(args.length);
    for (int i=0; i<args.length; i++) {
      // Set declared class of wrappers to to primitive type to work 
      // with ObjectWritable
      Class<? extends Object> declaredClass = args[i].getClass();
      if (declaredClass == Boolean.class)
        declaredClass = Boolean.TYPE;
      else if (declaredClass == Short.class)
        declaredClass = Short.TYPE;
      else if (declaredClass == Integer.class)
        declaredClass = Integer.TYPE;
      else if (declaredClass == Long.class)
        declaredClass = Long.TYPE;
      // Use ObjectWritable to serialize the parameter
      new ObjectWritable(declaredClass, args[i]).write(out);
    }
    byte[] bytes = bos.toByteArray();
    //LOG.info("RPC message length: " + bytes.length);
    FrameworkMessage msg = new FrameworkMessage(this.slaveId, 0, bytes);
    driver.sendFrameworkMessage(msg);
    
    // Wait for a reply
    synchronized(response) {
      while (!response.ready) {
        try {
          response.wait();
        } catch (InterruptedException e) {}
      }
    }
    
    //LOG.info("Result of " + method + ": " + response.result);
    
    rpcResponses.remove(rpcId);
    return response.result;
  }

  @Override
  public String getFilesystemName() throws IOException {
    try {
      return (String) invokeRPC("getFilesystemName");
    } catch (Exception e) {
      e.printStackTrace();
      System.exit(1);
      return null;
    }
  }

  @Override
  public String getSystemDir() {
    try {
      return (String) invokeRPC("getSystemDir");
    } catch (Exception e) {
      e.printStackTrace();
      System.exit(1);
      return null;
    }
  }

  @Override
  public TaskCompletionEvent[] getTaskCompletionEvents(JobID jobid,
      int fromEventId, int maxEvents) throws IOException {
    try {
      return (TaskCompletionEvent[]) invokeRPC("getTaskCompletionEvents", 
          jobid, fromEventId, maxEvents);
    } catch (Exception e) {
      e.printStackTrace();
      System.exit(1);
      return null;
    }
  }

  @Override
  public HeartbeatResponse heartbeat(TaskTrackerStatus status,
      boolean restarted, boolean initialContact, boolean acceptNewTasks,
      short responseId) throws IOException {
    try {
      // Expire any unmapped tasks that have been sitting idle for too long
      expireUnmappedTasks();
      
      // Look through statuses to see whether we have finished, failed or
      // killed any tasks. If so, notify Nexus through a status update and
      // decrease the slot counts on the tracker.
      // We add the status updates to a list and make sure to send it without
      // the NexusExecutor lock held so that we don't deadlock with the C++
      // code, which is calling into Java while holding the C++ scheduler lock.
      List<nexus.TaskStatus> updates = new ArrayList<nexus.TaskStatus>();
      synchronized (this) {
        for (TaskStatus taskStatus: status.getTaskReports()) {
          state = taskStatus.getRunState();
          if (state != TaskStatus.State.UNASSIGNED &&
              state != TaskStatus.State.RUNNING) {
            updates.add(createTaskDoneUpdate(taskStatus));
          }
        }
      }
      for (nexus.TaskStatus update: updates) {
        if (update != null) {
          driver.sendStatusUpdate(update);
        }
      }
      
      // Update the status to reflect the new slot counts; slot counts
      // may have decreased due to cleaning up unmapped tasks or due to
      // counting killed/failed/finished tasks above
      status.maxMapTasks = taskTracker.mapSlots;
      status.maxReduceTasks = taskTracker.reduceSlots;
      
      // Send the heartbeat and wait for a response
      HeartbeatResponse response = (HeartbeatResponse) invokeRPC("heartbeat",
          status, restarted, initialContact, acceptNewTasks, responseId);
      
      // Look through the response actions to see whether there are any
      // tasks to launch. If so, give them some Nexus task IDs so they can
      // later be identified and killed by Nexus.
      if (response != null && response.getActions() != null) {
        synchronized (this) {
          for (TaskTrackerAction action: response.getActions()) {
            if (action instanceof LaunchTaskAction) {
              LaunchTaskAction lta = (LaunchTaskAction) action;
              Task task = lta.getTask();
              TaskAttemptID hadoopId = task.getTaskID();
              Integer nexusId;
              if (task.isMapTask()) {
                if (unmappedMaps.isEmpty()) {
                  // This shouldn't happen due to the way we clean up
                  // unmapped tasks, but check for it.
                  throw new Exception("Launched Hadoop map task without "
                      + "having a corresponding Nexus task!");
                }
                nexusId = unmappedMaps.remove(0).taskId;
              } else {
                if (unmappedReduces.isEmpty()) {
                  // This shouldn't happen due to the way we clean up
                  // unmapped tasks, but check for it.
                  throw new Exception("Launched Hadoop reduce task without "
                      + "having a corresponding Nexus task!");
                }
                nexusId = unmappedReduces.remove(0).taskId;
              }
              LOG.info("Mapping " + hadoopId + " to " + nexusId);
              hadoopIdToNexusId.put(hadoopId, nexusId);
              nexusIdToHadoopId.put(nexusId, hadoopId);
            }
          }
        }
      }
      
      return response;
    } catch (Exception e) {
      e.printStackTrace();
      System.exit(1);
      return null;
    }
  }

  @Override
  public void reportTaskTrackerError(String taskTracker, String errorClass,
      String errorMessage) throws IOException {
    try {
      invokeRPC("reportTaskTrackerError", taskTracker, errorClass,
          errorMessage);
    } catch (Exception e) {
      e.printStackTrace();
      System.exit(1);
    }
  }

  @Override
  public long getProtocolVersion(String protocol, long clientVersion)
      throws IOException {
    try {
      return (Long) invokeRPC("getProtocolVersion", clientVersion);
    } catch (Exception e) {
      e.printStackTrace();
      System.exit(1);
      return 0;
    }
  }

  public synchronized nexus.TaskStatus createTaskDoneUpdate(
      TaskStatus taskStatus) {
    TaskAttemptID hadoopId = taskStatus.getTaskID();
    if (hadoopIdToNexusId.containsKey(hadoopId)) {
      Integer nexusId = hadoopIdToNexusId.get(hadoopId);
      TaskState nexusState;
      if (state == TaskStatus.State.SUCCEEDED ||
          state == TaskStatus.State.COMMIT_PENDING) {
        nexusState = TaskState.TASK_FINISHED;
      } else if (state == TaskStatus.State.FAILED ||
          state == TaskStatus.State.FAILED_UNCLEAN) {
        nexusState = TaskState.TASK_FAILED;
      } else { // state == KILLED or KILLED_UNCLEAN
        nexusState = TaskState.TASK_KILLED;
      }
      
      if (taskStatus.getIsMap()) {
        taskTracker.mapSlots--;
      } else {
        taskTracker.reduceSlots--;
      }
      
      nexus.TaskStatus update =
          new nexus.TaskStatus(nexusId, nexusState, new byte[0]);
      
      // Remove from hadoopIdToNexusId and nexusIdToHadoopId.
      // This will prevent us from sending a duplicate status update
      // about this task to Nexus in the future.
      hadoopIdToNexusId.remove(hadoopId);
      nexusIdToHadoopId.remove(nexusId);

      LOG.info("Unmapping " + hadoopId + " / " + nexusId);

      return update;
    }
    return null;
  }
  
  public ExecutorDriver getDriver() {
    return driver;
  }

  public static void main(String[] args) throws Exception {
    System.loadLibrary("nexus");
    new NexusExecutorDriver(new NexusExecutor()).run();
  }
}
