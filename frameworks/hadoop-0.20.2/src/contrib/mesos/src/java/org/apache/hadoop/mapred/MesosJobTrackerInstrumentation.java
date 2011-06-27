package org.apache.hadoop.mapred;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;

public class MesosJobTrackerInstrumentation extends JobTrackerInstrumentation {
  private final FrameworkScheduler scheduler;

  public MesosJobTrackerInstrumentation(
    JobTracker jt, JobConf conf, FrameworkScheduler scheduler
  ) { 
    super(jt, conf);
    this.scheduler = scheduler;
  }

  @Override
  public void failedMap(TaskAttemptID taskAttemptID) {
    scheduler.killedTask(taskAttemptID);
  }

  @Override
  public void failedReduce(TaskAttemptID taskAttemptID) {
    scheduler.killedTask(taskAttemptID);
  }
}
