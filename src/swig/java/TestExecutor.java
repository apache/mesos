import java.io.File;
import nexus.*;

public class TestExecutor extends Executor {
  static {
    System.loadLibrary("nexus");
  }

  @Override
  public void startTask(final TaskDescription task) {
    new Thread() { public void run() {
        System.out.println("Running task " + task.getTaskId());
        try { Thread.sleep(1000); } catch(Exception e) {}
        sendStatusUpdate(new TaskStatus(task.getTaskId(),
                                        TaskState.TASK_FINISHED,
                                        new byte[0]));
    }}.start();
  }

  public static void main(String[] args) throws Exception {
    TestExecutor exec = new TestExecutor();
    exec.run();
  }
}
