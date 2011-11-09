package org.apache.hadoop.mapred;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

import org.apache.mesos.MesosSchedulerDriver;
import org.apache.mesos.SchedulerDriver;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.mapred.TaskTrackerStatus;

public class MesosScheduler extends TaskScheduler {
  static {
    System.loadLibrary("mesos");
  }
  
  public static final Log LOG =
    LogFactory.getLog(MesosScheduler.class);
  
  private boolean running = false;
  private FrameworkScheduler frameworkScheduler;
  private SchedulerDriver driver;
  JobTracker jobTracker;

  private EagerTaskInitializationListener eagerInitListener;

  public MesosScheduler() { 
  }
  
  @Override
  public void start() throws IOException {
    try {
      LOG.info("Starting MesosScheduler");
      jobTracker = (JobTracker) super.taskTrackerManager;
      
      Configuration conf = getConf();
      String master = conf.get("mapred.mesos.master", "local");
      
      this.eagerInitListener = new EagerTaskInitializationListener(conf);
      eagerInitListener.setTaskTrackerManager(taskTrackerManager);
      eagerInitListener.start();
      taskTrackerManager.addJobInProgressListener(eagerInitListener);
      
      frameworkScheduler = new FrameworkScheduler(this); 
      driver = new MesosSchedulerDriver(frameworkScheduler,
          frameworkScheduler.getFrameworkName(),
          frameworkScheduler.getExecutorInfo(),
          master);
      
      driver.start();
    } catch (Exception e) {
      // If the MesosScheduler can't be loaded, the JT won't be useful at all,
      // so crash it now so that the user notices.
      LOG.fatal("Failed to start MesosScheduler", e);
      // TODO: Use System.exit(1) instead of RuntimeException?
      throw new RuntimeException("Failed to start MesosScheduler", e);
    }
  }

  @Override
  public void terminate() throws IOException {
    try {
      if (running) {
        LOG.info("Stopping MesosScheduler");
        driver.stop();
        frameworkScheduler.cleanUp();
      }
      if (eagerInitListener != null) {
        taskTrackerManager.removeJobInProgressListener(eagerInitListener);
      }
    } catch (Exception e) {
      e.printStackTrace();
    }
  }
  
  @Override
  public List<Task> assignTasks(TaskTrackerStatus taskTracker) throws IOException {
    return frameworkScheduler.assignTasks(taskTracker);
  }

  @Override
  public Collection<JobInProgress> getJobs(String queueName) {
    // TODO Actually return some jobs
    ArrayList<JobInProgress> list = new ArrayList<JobInProgress>();
    return list;
  }

}
