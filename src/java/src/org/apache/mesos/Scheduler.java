package org.apache.mesos;

import org.apache.mesos.Protos.*;

import java.util.List;


/**
 * Callback interface to be implemented by frameworks' schedulers.
 */
public interface Scheduler {
  public String getFrameworkName(SchedulerDriver driver);
  public ExecutorInfo getExecutorInfo(SchedulerDriver driver);
  public void registered(SchedulerDriver driver, FrameworkID frameworkId);
  public void resourceOffer(SchedulerDriver driver, OfferID offerId, List<SlaveOffer> offers);
  public void offerRescinded(SchedulerDriver driver, OfferID offerId);
  public void statusUpdate(SchedulerDriver driver, TaskStatus status);
  public void frameworkMessage(SchedulerDriver driver, SlaveID slaveId, ExecutorID executorId, byte[] data);
  public void slaveLost(SchedulerDriver driver, SlaveID slaveId);
  public void error(SchedulerDriver driver, int code, String message);
}
