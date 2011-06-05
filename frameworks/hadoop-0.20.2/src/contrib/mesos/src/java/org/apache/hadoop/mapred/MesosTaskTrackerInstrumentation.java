package org.apache.hadoop.mapred;

import java.io.File;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hadoop.mapred.TaskTracker.TaskInProgress;

public class MesosTaskTrackerInstrumentation extends TaskTrackerInstrumentation {
  public static final Log LOG =
    LogFactory.getLog(MesosTaskTrackerInstrumentation.class);
  
  private FrameworkExecutor executor;
  
  public MesosTaskTrackerInstrumentation(TaskTracker t) {
    super(t);
    executor = FrameworkExecutor.getInstance();
    if (executor == null) {
      throw new IllegalArgumentException("MesosTaskTrackerInstrumentation " +
          "is being used without an active FrameworkExecutor");
    }
  }
  
  @Override
  public void statusUpdate(Task task, TaskStatus taskStatus) {
    executor.statusUpdate(task, taskStatus);
  }
}
