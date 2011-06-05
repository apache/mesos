import java.io.FileWriter;
import mesos.*;

public class TestExecutor extends Executor {
  static {
    System.loadLibrary("mesos");
  }

  @Override
  public void launchTask(final ExecutorDriver d, final TaskDescription task) {
    new Thread() { public void run() {
      try {
        System.out.println("Running task " + task.getTaskId());
        FileWriter outputStream = new FileWriter("test_file.txt");
        outputStream.write("test output text to write", 0, 25);
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
    ExecutorDriver driver = new MesosExecutorDriver(exec);
    driver.run();
  }
}
