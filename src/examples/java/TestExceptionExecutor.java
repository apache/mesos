import java.io.File;
import mesos.*;

public class TestExceptionExecutor extends Executor {
  static {
    System.loadLibrary("mesos");
  }

  @Override
  public void launchTask(final ExecutorDriver d, final TaskDescription task) {
    new Thread() { public void run() {
      System.out.println("About to throw exception in launchTask");
      int[] errorArray = {1,2};
      int myInt = errorArray[2];
      System.out.println("Running task " + task.getTaskId());
    }}.start();
  }

  public static void main(String[] args) throws Exception {
    TestExceptionExecutor exec = new TestExceptionExecutor();
    ExecutorDriver driver = new MesosExecutorDriver(exec);
    driver.run();
  }
}
