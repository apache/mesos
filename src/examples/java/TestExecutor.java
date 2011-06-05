import java.io.File;

import org.apache.mesos.*;
import org.apache.mesos.Protos.*;


public class TestExecutor implements Executor {
  @Override
  public void init(ExecutorDriver driver, ExecutorArgs args) {}

  @Override
  public void launchTask(final ExecutorDriver driver, final TaskDescription task) {
    new Thread() { public void run() {
      try {
        System.out.println("Running task " + task.getTaskId());

        if (task.hasData()) {
          Thread.sleep(Integer.parseInt(task.getData().toStringUtf8()));
        } else {
          Thread.sleep(1000);
        }

        TaskStatus status = TaskStatus.newBuilder()
          .setTaskId(task.getTaskId())
          .setSlaveId(task.getSlaveId())
          .setState(TaskState.TASK_FINISHED).build();

        driver.sendStatusUpdate(status);
      } catch (Exception e) {
        e.printStackTrace();
      }
    }}.start();
  }

  @Override
  public void killTask(ExecutorDriver driver, TaskID taskId) {}

  @Override
  public void frameworkMessage(ExecutorDriver driver, byte[] data) {}

  @Override
  public void shutdown(ExecutorDriver driver) {}

  @Override
  public void error(ExecutorDriver driver, int code, String message) {}

  public static void main(String[] args) throws Exception {
    new MesosExecutorDriver(new TestExecutor()).run();
  }
}
