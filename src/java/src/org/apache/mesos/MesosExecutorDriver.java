package org.apache.mesos;

import org.apache.mesos.Protos.*;


/**
 * Concrete implementation of ExecutorDriver that communicates with a
 * Mesos slave. The slave's location is read from environment variables
 * set by it when it execs the user's executor script; users only need
 * to create the MesosExecutorDriver and call run() on it.
 */
public class MesosExecutorDriver implements ExecutorDriver {
  static {
    System.loadLibrary("mesos");
  }

  public MesosExecutorDriver(Executor exec) {
    this.exec = exec;

    initialize();
  }

  // Lifecycle methods.
  public native int start();
  public native int stop();
  public native int join();

  public int run() {
    int ret = start();
    return ret != 0 ? ret : join();
  }

  public native int sendStatusUpdate(TaskStatus status);
  public native int sendFrameworkMessage(FrameworkMessage message);

  protected native void initialize();
  protected native void finalize();

  private final Executor exec;
  
  private long __exec;
  private long __driver;
}

