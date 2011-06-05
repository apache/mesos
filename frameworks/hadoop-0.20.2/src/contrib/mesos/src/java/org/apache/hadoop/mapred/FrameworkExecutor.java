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

import mesos.Executor;
import mesos.ExecutorArgs;
import mesos.ExecutorDriver;
import mesos.FrameworkMessage;
import mesos.MesosExecutorDriver;
import mesos.TaskDescription;
import mesos.TaskState;

public class FrameworkExecutor extends Executor {
  static {
    System.loadLibrary("mesos");
  }
  
  private static FrameworkExecutor instance;
  
  public static final Log LOG =
    LogFactory.getLog(FrameworkExecutor.class);
  
  private ExecutorDriver driver;
  private String slaveId;
  private JobConf conf;
  private TaskTracker taskTracker;

  private Set<Integer> activeMesosTasks = new HashSet<Integer>();

  @Override
  public void init(ExecutorDriver d, ExecutorArgs args) {
    try {
      this.driver = d;
      slaveId = args.getSlaveId();
      
      // TODO: initialize all of JobConf from ExecutorArgs (using JT's conf)?
      conf = new JobConf();
      String jobTracker = new String(args.getData());
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
      conf.set("slave.host.name", args.getHost());
      
      taskTracker = new TaskTracker(conf);
      new Thread("TaskTracker run thread") {
        @Override
        public void run() {
          taskTracker.run();
        }
      }.start();
    } catch (Exception e) {
      LOG.fatal("Failed to initialize FrameworkExecutor", e);
      System.exit(1);
    }
  }
  
  @Override
  public void launchTask(ExecutorDriver d, TaskDescription task) {
    LOG.info("Asked to launch Mesos task " + task.getTaskId());
    activeMesosTasks.add(task.getTaskId());
  }
  
  @Override
  public void killTask(ExecutorDriver d, int taskId) {
    LOG.info("Asked to kill Mesos task " + taskId);
    // TODO: Tell the JobTracker about this using an E2S_KILL_REQUEST message!
  }
  
  @Override
  public void frameworkMessage(ExecutorDriver d, FrameworkMessage msg) {
    try {
      int mesosId = msg.getTaskId();
      HadoopFrameworkMessage hfm = new HadoopFrameworkMessage(msg.getData());
      switch (hfm.type) {
        case S2E_SEND_STATUS_UPDATE: {
          TaskState s = TaskState.valueOf(hfm.arg1);
          LOG.info("Sending status update: " + mesosId + " is " + s);
          d.sendStatusUpdate(new mesos.TaskStatus(mesosId, s, new byte[0]));
          break;
        }
        case S2E_SHUTDOWN_EXECUTOR: {
          taskTracker.close();
          System.exit(0);
        }
      }
    } catch (IOException e) {
      LOG.fatal("Failed to deserialize HadoopFrameworkMessage", e);
      System.exit(1);
    }
  }
  
  public void statusUpdate(Task task, TaskStatus status) {
    LOG.info("Status update: " + task.getTaskID() + " is " + 
        status.getRunState());
    if (!task.extraData.equals("")) {
      // Parse Mesos ID from extraData
      int mesosId = Integer.parseInt(task.extraData);
      if (activeMesosTasks.contains(mesosId)) {
        // Check whether the task has finished (either successfully or not),
        // and report to Mesos if it has
        State state = status.getRunState();
        TaskState mesosState = null;
        if (state == State.SUCCEEDED || state == State.COMMIT_PENDING)
          mesosState = TaskState.TASK_FINISHED;
        else if (state == State.FAILED || state == State.FAILED_UNCLEAN)
          mesosState = TaskState.TASK_FAILED;
        else if (state == State.KILLED || state == State.KILLED_UNCLEAN)
          mesosState = TaskState.TASK_KILLED;
        if (mesosState != null) {
          driver.sendStatusUpdate(
              new mesos.TaskStatus(mesosId, mesosState, new byte[0]));
          activeMesosTasks.remove(mesosId);
        }
      }
    }
  }

  public static void main(String[] args) {
    instance = new FrameworkExecutor();
    new MesosExecutorDriver(instance).run();
  }
  
  static FrameworkExecutor getInstance() {
    return instance;
  }
}
