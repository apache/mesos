import java.io.File;

import java.util.List;

import mesos.*;
import mesos.Protos.*;


public class TestExceptionFramework {
  static class MyScheduler implements Scheduler {
    @Override
    public String getFrameworkName(SchedulerDriver driver) {
      throw new ArrayIndexOutOfBoundsException();
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
    public void registered(SchedulerDriver driver, FrameworkID frameworkId) {}

    @Override
    public void resourceOffer(SchedulerDriver driver,
                              OfferID offerId,
                              List<SlaveOffer> offers) {}

    @Override
    public void offerRescinded(SchedulerDriver driver, OfferID offerId) {}

    @Override
    public void statusUpdate(SchedulerDriver driver, TaskStatus status) {}

    @Override
    public void frameworkMessage(SchedulerDriver driver, FrameworkMessage message) {}

    @Override
    public void slaveLost(SchedulerDriver driver, SlaveID slaveId) {}

    @Override
    public void error(SchedulerDriver driver, int code, String message) {}
  }

  public static void main(String[] args) throws Exception {
    new MesosSchedulerDriver(new MyScheduler(), args[0]).run();
  }
}
