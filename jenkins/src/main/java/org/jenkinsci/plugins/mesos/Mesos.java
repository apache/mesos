package org.jenkinsci.plugins.mesos;

public abstract class Mesos {
  private static MesosImpl mesos;

  public static class JenkinsSlave {
    String name;

    public JenkinsSlave(String name) {
      this.name = name;
    }
  }

  public static class SlaveRequest {
    JenkinsSlave slave;
    int executors;

    public SlaveRequest(JenkinsSlave _slave, int _executors) {
      this.slave = _slave;
      this.executors = _executors;
    }
  }

  interface SlaveResult {
    void running(JenkinsSlave slave);

    void finished(JenkinsSlave slave);

    void failed(JenkinsSlave slave);
  }

  abstract public void startScheduler(String jenkinsMaster, String mesosMaster);
  abstract public boolean isSchedulerRunning();
  abstract public void stopScheduler();

  /**
   * Starts a jenkins slave asynchronously in the mesos cluster.
   *
   * @param request
   *          slave request.
   * @param result
   *          this callback will be called when the slave starts.
   */
  abstract public void startJenkinsSlave(SlaveRequest request, SlaveResult result);


  /**
   * Stop a jenkins slave asynchronously in the mesos cluster.
   *
   * @param name
   *          jenkins slave.
   *
   */
  abstract public void stopJenkinsSlave(String name);

  /**
   * @return the mesos implementation instance
   */
  public static synchronized Mesos getInstance() {
    if (mesos == null) {
      mesos = new MesosImpl();
    }
    return mesos;
  }

  public static class MesosImpl extends Mesos {
    @Override
    public synchronized void startScheduler(String jenkinsMaster, String mesosMaster) {
      stopScheduler();
      scheduler = new JenkinsScheduler(jenkinsMaster, mesosMaster);
      scheduler.init();
    }

    @Override
    public synchronized boolean isSchedulerRunning() {
      return scheduler != null && scheduler.isRunning();
    }

    @Override
    public synchronized void stopScheduler() {
      if (scheduler != null) {
        scheduler.stop();
        scheduler = null;
      }
    }

    @Override
    public synchronized void startJenkinsSlave(SlaveRequest request, SlaveResult result) {
      if (scheduler != null) {
        scheduler.requestJenkinsSlave(request, result);
      }
    }

    @Override
    public synchronized void stopJenkinsSlave(String name) {
      if (scheduler != null) {
        scheduler.terminateJenkinsSlave(name);
      }
    }

    private JenkinsScheduler scheduler;
  }
}
