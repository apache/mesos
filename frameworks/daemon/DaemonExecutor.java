import java.io.File;

import nexus.*;

public class DaemonExecutor extends Executor {
  static {
    System.loadLibrary("nexus");
  }

  @Override
  public void startTask(final TaskDescription task) {
    System.out.println("Running task " + task.getName());
    try {
      Thread.sleep(100000);
    } catch (InterruptedException ie) { ie.printStackTrace(); }
    TaskStatus status = new TaskStatus(task.getTaskId(),
				       TaskState.TASK_FAILED,
				       new byte[0]);
    sendStatusUpdate(status);
  }

  

  public static void main(String[] args) throws Exception {
    new DaemonExecutor().run();
  }
}
