import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import mesos.*;

public class TestFramework {
  static {
    System.loadLibrary("mesos");
  }

  static class MyScheduler extends Scheduler {
    int launchedTasks = 0;
    int finishedTasks = 0;
    int totalTasks = 5;

    public MyScheduler() {}

    public MyScheduler(int numTasks) {
      totalTasks = numTasks;
    }

    @Override
    public String getFrameworkName(SchedulerDriver d) {
      return "Java test framework";
    }

    @Override
    public ExecutorInfo getExecutorInfo(SchedulerDriver d) {
      try {
        // Get the location where the Java test framework is installed, so
        // that we can give a path to our executor. Our wrapper script
        // (test_framework) sets this in environment variable FRAMEWORK_DIR.
        String frameworkDir = System.getenv("FRAMEWORK_DIR");
        if (frameworkDir == null) {
          throw new Exception("FRAMEWORK_DIR environment variable is not set");
        }
        return new ExecutorInfo(
            new File(frameworkDir, "test_executor").getCanonicalPath(),
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
                              List<SlaveOffer> offers) {
      System.out.println("Got offer " + oid);
      List<TaskDescription> tasks = new ArrayList<TaskDescription>();
      for (SlaveOffer offer: offers) {
        System.out.println("Params for slave " + offer.getSlaveId() + ":");
        for (Map.Entry<String, String> entry: offer.getParams().entrySet()) {
          System.out.println(entry.getKey() + " = " + entry.getValue());
        }
        if (launchedTasks < totalTasks) {
          int taskId = launchedTasks++;
          Map<String, String> taskParams = new HashMap<String, String>();
          taskParams.put("cpus", "1");
          taskParams.put("mem", "128");
          System.out.println("Launching task " + taskId);
          tasks.add(new TaskDescription(taskId,
                                        offer.getSlaveId(),
                                        "task " + taskId,
                                        taskParams,
                                        new byte[0]));
        }
      }
      Map<String, String> params = new HashMap<String, String>();
      params.put("timeout", "1");
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
    if (args.length < 1 || args.length > 2) {
      System.err.println("Usage: test_framework <master> [<numTasks>]");
    } else if (args.length == 1) {
      new MesosSchedulerDriver(new MyScheduler(), args[0]).run();
    } else {
      new MesosSchedulerDriver(new MyScheduler(Integer.parseInt(args[1])),
                               args[0]).run();
    }
  }
}
