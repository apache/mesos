package mesos;

import mesos.Protos.*;


/**
 * Callback interface to be implemented by frameworks' executors.
 */
public interface Executor {
  public void init(ExecutorDriver driver, ExecutorArgs args);
  public void launchTask(ExecutorDriver driver, TaskDescription task);
  public void killTask(ExecutorDriver driver, TaskID taskId);
  public void frameworkMessage(ExecutorDriver driver, FrameworkMessage message);
  public void shutdown(ExecutorDriver driver);
  public void error(ExecutorDriver driver, int code, String message);
}
