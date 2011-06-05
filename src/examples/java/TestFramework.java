import java.io.File;
import java.io.IOException;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import mesos.*;
import mesos.Protos.*;


public class TestFramework {
  static class MyScheduler implements Scheduler {
    int launchedTasks = 0;
    int finishedTasks = 0;
    int totalTasks = 5;

    public MyScheduler() {}

    public MyScheduler(int numTasks) {
      totalTasks = numTasks;
    }

    @Override
    public String getFrameworkName(SchedulerDriver driver) {
      return "Java test framework";
    }

    @Override
    public ExecutorInfo getExecutorInfo(SchedulerDriver driver) {
      try {
        File file = new File("./test_executor");
        return ExecutorInfo.newBuilder()
          .setUri(file.getCanonicalPath()).build();
      } catch (Throwable t) {
        throw new RuntimeException(t);
      }
    }

    @Override
    public void registered(SchedulerDriver driver, FrameworkID frameworkId) {
      System.out.println("Registered! ID = " + frameworkId.getValue());
    }

    @Override
    public void resourceOffer(SchedulerDriver driver,
                              OfferID offerId,
                              List<SlaveOffer> offers) {
      System.out.println("Got offer " + offerId.getValue());
      List<TaskDescription> tasks = new ArrayList<TaskDescription>();

      for (SlaveOffer offer : offers) {
        if (launchedTasks < totalTasks) {
          TaskID taskId = TaskID.newBuilder()
            .setValue(Integer.toString(launchedTasks++)).build();

          System.out.println("Launching task " + taskId.getValue());

          TaskDescription task = TaskDescription.newBuilder()
            .setName("task " + taskId.getValue())
            .setTaskId(taskId)
            .setSlaveId(offer.getSlaveId())
            .setParams(Params.newBuilder()
                       .addParam(Param.newBuilder()
                                 .setKey("cpus")
                                 .setValue("1").build())
                       .addParam(Param.newBuilder()
                                 .setKey("mem")
                                 .setValue("128").build()).build()).build();

          tasks.add(task);
        }
      }

      Map<String, String> params = new HashMap<String, String>();
      params.put("timeout", "1");
      driver.replyToOffer(offerId, tasks, params);
    }

    @Override
    public void offerRescinded(SchedulerDriver driver, OfferID offerId) {}

    @Override
    public void statusUpdate(SchedulerDriver driver, TaskStatus status) {
      System.out.println("Status update: task " + status.getTaskId() +
                         " is in state " + status.getState());
      if (status.getState() == TaskState.TASK_FINISHED) {
        finishedTasks++;
        System.out.println("Finished tasks: " + finishedTasks);
        if (finishedTasks == totalTasks)
          driver.stop();
      }
    }

    @Override
    public void frameworkMessage(SchedulerDriver driver, FrameworkMessage message) {}

    @Override
    public void slaveLost(SchedulerDriver driver, SlaveID slaveId) {}

    @Override
    public void error(SchedulerDriver driver, int code, String message) {
      System.out.println("Error: " + message);
    }
  }

  public static void main(String[] args) throws Exception {
    if (args.length < 1 || args.length > 2) {
      System.out.println("Invalid use: please specify a master");
    } else if (args.length == 1) {
      new MesosSchedulerDriver(new MyScheduler(),args[0]).run();
    } else {
      new MesosSchedulerDriver(new MyScheduler(Integer.parseInt(args[1])), args[0]).run();
    }
  }
}
