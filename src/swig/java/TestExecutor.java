import java.io.File;
import nexus.*;

public class TestExecutor extends Executor {
  static {
    System.loadLibrary("nexus");
  }

  @Override
  public void launchTask(final ExecutorDriver d, final TaskDescription task) {
    new Thread() { public void run() {
      try {
        System.out.println("Running task " + task.getTaskId());
        Thread.sleep(1000);
        d.sendStatusUpdate(new TaskStatus(task.getTaskId(),
                                          TaskState.TASK_FINISHED,
                                          new byte[0]));
      } catch (Exception e) {
        e.printStackTrace();
      }
    }}.start();
  }

  public static void main(String[] args) throws Exception {
    TestExecutor exec = new TestExecutor();
    ExecutorDriver driver = new NexusExecutorDriver(exec);
    driver.run();
  }
}
