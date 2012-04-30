package org.apache.hadoop.mapred;

import java.io.IOException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.Map.Entry;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hadoop.mapred.TaskStatus.State;
import org.apache.hadoop.mapred.TaskTracker.TaskInProgress;
import org.hsqldb.lib.Iterator;

import org.apache.mesos.Executor;
import org.apache.mesos.ExecutorDriver;
import org.apache.mesos.MesosExecutorDriver;
import org.apache.mesos.Protos.ExecutorInfo;
import org.apache.mesos.Protos.FrameworkID;
import org.apache.mesos.Protos.FrameworkInfo;
import org.apache.mesos.Protos.SlaveID;
import org.apache.mesos.Protos.SlaveInfo;
import org.apache.mesos.Protos.Status;
import org.apache.mesos.Protos.TaskID;
import org.apache.mesos.Protos.TaskInfo;
import org.apache.mesos.Protos.TaskState;

public class FrameworkExecutor implements Executor {
  public static final Log LOG =
    LogFactory.getLog(FrameworkExecutor.class);

  private static FrameworkExecutor instance;

  private ExecutorDriver driver;
  private SlaveID slaveId;
  private JobConf conf;
  private TaskTracker taskTracker;

  private Set<TaskID> activeMesosTasks = new HashSet<TaskID>();

  @Override
  public void registered(ExecutorDriver d,
                         ExecutorInfo executorInfo,
                         FrameworkInfo frameworkInfo,
                         SlaveInfo slaveInfo) {
    try {
      Thread.currentThread().setContextClassLoader(
        TaskTracker.class.getClassLoader());

      this.driver = d;
      this.slaveId = slaveId;

      // TODO: initialize all of JobConf from ExecutorArgs (using JT's conf)?
      conf = new JobConf();
      String jobTracker = executorInfo.getData().toStringUtf8();
      LOG.info("Setting JobTracker: " + jobTracker);
      conf.set("mapred.job.tracker", jobTracker);

      // Attach our TaskTrackerInstrumentation to figure out when tasks end
      Class<?>[] instClasses = TaskTracker.getInstrumentationClasses(conf);
      String newInstClassList = "";
      for (Class<?> cls: instClasses) {
        newInstClassList += cls.getName() + ",";
      }
      newInstClassList += MesosTaskTrackerInstrumentation.class.getName();
      conf.set("mapred.tasktracker.instrumentation", newInstClassList);

      // Get hostname from Mesos to make sure we match what it reports to the JT
      conf.set("slave.host.name", slaveInfo.getHostname());

      taskTracker = new TaskTracker(conf);
      new Thread("TaskTracker run thread") {
        @Override
        public void run() {
          taskTracker.run();
        }
      }.start();
    } catch (Exception e) {
      throw new RuntimeException(
          "Failed to initialize FrameworkExecutor", e);
    }
  }

  @Override
  public void reregistered(ExecutorDriver driver, SlaveInfo slaveInfo) {}

  @Override
  public void disconnected(ExecutorDriver driver) {}

  @Override
  public void launchTask(ExecutorDriver d, TaskInfo task) {
    LOG.info("Asked to launch Mesos task " + task.getTaskId().getValue());
    activeMesosTasks.add(task.getTaskId());
  }

  @Override
  public void killTask(ExecutorDriver d, TaskID taskId) {
    LOG.info("Asked to kill Mesos task " + taskId);
    // TODO: Tell the JobTracker about this using an E2S_KILL_REQUEST message!
  }

  @Override
  public void frameworkMessage(ExecutorDriver d, byte[] msg) {
    try {
      HadoopFrameworkMessage hfm = new HadoopFrameworkMessage(msg);
      switch (hfm.type) {
        case S2E_SEND_STATUS_UPDATE: {
          TaskState s = TaskState.valueOf(hfm.arg1);
          LOG.info("Sending status update: " + hfm.arg2 + " is " + s);
          d.sendStatusUpdate(org.apache.mesos.Protos.TaskStatus.newBuilder()
                             .setTaskId(TaskID.newBuilder()
                                        .setValue(hfm.arg2).build())
                             .setState(s).build());
          break;
        }
        case S2E_SHUTDOWN_EXECUTOR: {
          taskTracker.close();
          System.exit(0);
        }
      }
    } catch (Exception e) {
      throw new RuntimeException(
          "Failed to deserialize HadoopFrameworkMessage", e);
    }
  }

  public void statusUpdate(Task task, TaskStatus status) {
    // There are some tasks that get launched implicitly (e.g., the
    // setup/cleanup tasks) that don't go through the
    // MesosScheduler/FrameworkScheduler.assignTasks and thus don't
    // get extraData set properly! This means WE DO NOT EVER SEND
    // STATUS UPDATES FOR THESE TASKS. For this reason we also don't
    // need to specify any resources for the executor because Mesos
    // always assumes these tasks are running.
    if (task.extraData.equals("")) {
      LOG.info("Ignoring status update for task " + task);
      return;
    }

    // Create a Mesos TaskID from extraData.
    TaskID taskId = TaskID.newBuilder()
      .setValue(task.extraData)
      .build();

    // It appears as though we can get multiple duplicate status
    // updates for the same task, so check if we still have an active
    // task so that we only send the status update once.
    if (!activeMesosTasks.contains(taskId)) {
      LOG.info("Ignoring (duplicate) status update for task " + task);
      return;
    }

    // Check whether the task has finished (either successfully or
    // not), and report to Mesos only if it has.
    State state = status.getRunState();
    TaskState mesosState = null;
    if (state == State.SUCCEEDED || state == State.COMMIT_PENDING)
      mesosState = TaskState.TASK_FINISHED;
    else if (state == State.FAILED || state == State.FAILED_UNCLEAN)
      mesosState = TaskState.TASK_FAILED;
    else if (state == State.KILLED || state == State.KILLED_UNCLEAN)
      mesosState = TaskState.TASK_KILLED;

    if (mesosState == null) {
      LOG.info("Not sending status update for task " + task +
               " in state " + state);
      return;
    }

    LOG.info("Attempting to send status update for " + task +
             " in state " + status.getRunState());

    driver.sendStatusUpdate(
        org.apache.mesos.Protos.TaskStatus.newBuilder()
        .setTaskId(TaskID.newBuilder().setValue(task.extraData).build())
        .setState(mesosState)
        .build());

    activeMesosTasks.remove(taskId);
  }

  @Override
  public void error(ExecutorDriver d, String message) {
    LOG.error("FrameworkExecutor.error: " + message);
  }

  @Override
  public void shutdown(ExecutorDriver d) {}

  public static void main(String[] args) {
    instance = new FrameworkExecutor();
    MesosExecutorDriver driver = new MesosExecutorDriver(instance);
    System.exit(driver.run() == Status.DRIVER_STOPPED ? 0 : 1);
  }

  static FrameworkExecutor getInstance() {
    return instance;
  }
}
