import java.io.File;
import mesos.*;

public class TestExceptionFramework {
  static {
    System.loadLibrary("mesos");
  }

  static class MyScheduler extends Scheduler {
    int launchedTasks = 0;
    int finishedTasks = 0;
    final int totalTasks = 5;

    @Override
    public String getFrameworkName(SchedulerDriver d) {
      System.out.println("About to throw exception in getFrameworkName");
      int[] errorArray = {1,2};
      int myInt = errorArray[2];
      return "TestException Framework";
    }

    @Override
    public ExecutorInfo getExecutorInfo(SchedulerDriver d) {
      try {
        return new ExecutorInfo(
            new File("./test_executor").getCanonicalPath(),
            new byte[0]);
      } catch (Exception e) {
        e.printStackTrace();
        System.exit(1);
        return null;
      }
    }

    @Override
    public void registered(SchedulerDriver d, String fid) {
      System.out.println("Registered! FID = " + fid);
    }

    @Override
    public void resourceOffer(SchedulerDriver d,
                              String oid,
                              SlaveOfferVector offers) {
      System.out.println("Got offer offer " + oid);
      TaskDescriptionVector tasks = new TaskDescriptionVector();
      if (launchedTasks < totalTasks) {
        SlaveOffer offer = offers.get(0);
        int taskId = launchedTasks++;
        StringMap taskParams = new StringMap();
        taskParams.set("cpus", "1");
        taskParams.set("mem", "32");
        System.out.println("Launching task " + taskId);
        tasks.add(new TaskDescription(launchedTasks,
                                      offer.getSlaveId(),
                                      "task " + taskId,
                                      taskParams,
                                      new byte[0]));
	launchedTasks++;
      }
      StringMap params = new StringMap();
      params.set("timeout", "1");
      d.replyToOffer(oid, tasks, params);
    }

    @Override
    public void statusUpdate(SchedulerDriver d, TaskStatus status) {
      System.out.println("Status update: task " + status.getTaskId() +
                         " is in state " + status.getState());
      if (status.getState() == TaskState.TASK_FINISHED) {
        finishedTasks++;
        System.out.println("Finished tasks: " + finishedTasks);
        if (finishedTasks == totalTasks)
          d.stop();
      }
    }

    @Override
    public void error(SchedulerDriver d, int code, String message) {
      System.out.println("Error: " + message);
    }
  }

  public static void main(String[] args) throws Exception {
    new MesosSchedulerDriver(new MyScheduler(), args[0]).run();
    // TODO: Java should just exit here, and it does so on Linux, but
    // it doesn't on OS X. We should figure out why. It may have to do
    // with the libprocess threads being around and not being marked
    // as daemon threads by the JVM.
  }
}
