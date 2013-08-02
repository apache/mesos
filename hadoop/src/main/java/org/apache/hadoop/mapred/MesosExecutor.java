package org.apache.hadoop.mapred;

import java.io.IOException;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.mesos.Executor;
import org.apache.mesos.ExecutorDriver;
import org.apache.mesos.MesosExecutorDriver;
import org.apache.mesos.Protos.Environment.Variable;
import org.apache.mesos.Protos.ExecutorInfo;
import org.apache.mesos.Protos.FrameworkInfo;
import org.apache.mesos.Protos.SlaveInfo;
import org.apache.mesos.Protos.Status;
import org.apache.mesos.Protos.TaskID;
import org.apache.mesos.Protos.TaskInfo;
import org.apache.mesos.Protos.TaskState;
import org.apache.mesos.Protos.TaskStatus;

public class MesosExecutor implements Executor {
  public static final Log LOG = LogFactory.getLog(MesosExecutor.class);

  private JobConf conf;
  private TaskTracker taskTracker;

  @Override
  public void registered(ExecutorDriver driver, ExecutorInfo executorInfo,
      FrameworkInfo frameworkInfo, SlaveInfo slaveInfo) {
    LOG.info("Executor registered with the slave");

    conf = new JobConf();

    // Get TaskTracker's config options from environment variables set by the
    // JobTracker.
    if (executorInfo.getCommand().hasEnvironment()) {
      for (Variable variable : executorInfo.getCommand().getEnvironment()
          .getVariablesList()) {
        LOG.info("Setting config option : " + variable.getName() + " to "
            + variable.getValue());
        conf.set(variable.getName(), variable.getValue());
      }
    }

    // Get hostname from Mesos to make sure we match what it reports
    // to the JobTracker.
    conf.set("slave.host.name", slaveInfo.getHostname());

    // Set the mapred.local directory inside the executor sandbox, so that
    // different TaskTrackers on the same host do not step on each other.
    conf.set("mapred.local.dir", System.getProperty("user.dir") + "/mapred");
  }

  @Override
  public void launchTask(final ExecutorDriver driver, final TaskInfo task) {
    LOG.info("Launching task : " + task.getTaskId().getValue());

    // NOTE: We need to manually set the context class loader here because,
    // the TaskTracker is unable to find LoginModule class otherwise.
    Thread.currentThread().setContextClassLoader(
        TaskTracker.class.getClassLoader());

    try {
      taskTracker = new TaskTracker(conf);
    } catch (IOException e) {
      LOG.fatal("Failed to start TaskTracker", e);
      System.exit(1);
    } catch (InterruptedException e) {
      LOG.fatal("Failed to start TaskTracker", e);
      System.exit(1);
    }

    // Spin up a TaskTracker in a new thread.
    new Thread("TaskTracker Run Thread") {
      @Override
      public void run() {
        taskTracker.run();

        // Send a TASK_FINISHED status update.
        // We do this here because we want to send it in a separate thread
        // than was used to call killTask().
        driver.sendStatusUpdate(TaskStatus.newBuilder()
            .setTaskId(task.getTaskId())
            .setState(TaskState.TASK_FINISHED)
            .build());

        // Give some time for the update to reach the slave.
        try {
          Thread.sleep(1000);
        } catch (InterruptedException e) {
          LOG.error("Failed to sleep TaskTracker thread", e);
        }

        // Stop the executor.
        driver.stop();
      }
    }.start();

    driver.sendStatusUpdate(TaskStatus.newBuilder()
        .setTaskId(task.getTaskId())
        .setState(TaskState.TASK_RUNNING).build());
  }

  @Override
  public void killTask(ExecutorDriver driver, TaskID taskId) {
    LOG.info("Killing task : " + taskId.getValue());
    try {
      taskTracker.shutdown();
    } catch (IOException e) {
      LOG.error("Failed to shutdown TaskTracker", e);
    } catch (InterruptedException e) {
      LOG.error("Failed to shutdown TaskTracker", e);
    }
  }

  @Override
  public void reregistered(ExecutorDriver driver, SlaveInfo slaveInfo) {
    LOG.info("Executor reregistered with the slave");
  }

  @Override
  public void disconnected(ExecutorDriver driver) {
    LOG.info("Executor disconnected from the slave");
  }

  @Override
  public void frameworkMessage(ExecutorDriver d, byte[] msg) {
    LOG.info("Executor received framework message of length: " + msg.length
        + " bytes");
  }

  @Override
  public void error(ExecutorDriver d, String message) {
    LOG.error("MesosExecutor.error: " + message);
  }

  @Override
  public void shutdown(ExecutorDriver d) {
    LOG.info("Executor asked to shutdown");
  }

  public static void main(String[] args) {
    MesosExecutorDriver driver = new MesosExecutorDriver(new MesosExecutor());
    System.exit(driver.run() == Status.DRIVER_STOPPED ? 0 : 1);
  }
}
