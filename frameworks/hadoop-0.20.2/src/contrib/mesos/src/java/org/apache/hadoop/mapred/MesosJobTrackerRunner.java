package org.apache.hadoop.mapred;

import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.text.DateFormat;
import java.text.SimpleDateFormat;
import java.util.Date;

import org.apache.hadoop.fs.FSDataInputStream;
import org.apache.hadoop.fs.FSDataOutputStream;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.fs.Path;

/**
 * Executes a JobTracker with a given tag for the Mesos JobTracker launcher
 * system, maintaining the correct lease through HDFS files and potentially
 * shutting down the JT after a period of inactivity. 
 * 
 * TODO: Still need to implement timeout functionality.
 */
public class MesosJobTrackerRunner {
  // Date format to use in JobTracker identifiers
  static DateFormat DATE_FORMAT = new SimpleDateFormat("yyyyMMddHHmm");
  
  private static class LeaseRenewalThread extends Thread {
    private final FileSystem fs;
    private final Path lockPath;
    private final long leaseTimeout;
    private final JobTracker jt;

    public LeaseRenewalThread(
        FileSystem fs, Path lockPath, long leaseTimeout, JobTracker jt) {
      super("LeaseRenewalThread");
      this.fs = fs;
      this.lockPath = lockPath;
      this.leaseTimeout = leaseTimeout;
      this.jt = jt;
      setDaemon(true);
    }
    
    @Override
    public void run() {
      while (true) {
        try {
          fs.setTimes(lockPath, -1, System.currentTimeMillis());
        } catch (IOException e) {
          // If we failed to set the path, something is seriously wrong
          // (for example, the lease file was deleted); stop the JT and exit
          System.err.println("Fatal error: failed to setTimes on " + lockPath);
          e.printStackTrace();
          try {
            jt.stopTracker();
          } catch (IOException e2) {}
          System.exit(0);
        }
        try {
          Thread.sleep(leaseTimeout / 4);
        } catch (InterruptedException e) {}
      }
    }
  }
  
  public static void main(String[] args) throws IOException {
    if (args.length != 1) {
      System.err.println("Usage: MesosJobTrackerRunner <tag>");
      System.exit(1);
    }
    String tag = args[0];
    
    JobConf conf = new JobConf();
    FileSystem fs = FileSystem.get(conf);

    // Read configuration options
    String launcherDir = conf.get("mesos.launcher.dir", "/user/mesos/launcher");
    long leaseTimeout = conf.getLong("mesos.launcher.lease.timeout", 30000);
    
    Path lockPath = new Path(launcherDir, tag + ".lock");
    Path jtPath = new Path(launcherDir, tag + ".jobtracker");
    Path webuiPath = new Path(launcherDir, tag + ".webui");
    
    // Delete the .lock, .jobtracker and .webui files on exit
    fs.deleteOnExit(lockPath);
    
    // Start the JobTracker
    try {
      // Create a new JobTracker ID - make sure to include the username
      // Note: The task status server on the TT seems to break if we add an
      // underscore into the JobTracker ID, so we use a dash instead
      String jtId = DATE_FORMAT.format(new Date()) + "-" + tag;
      // Configure the JT to bind to random ports for RPC and web UI
      JobConf jtConf = new JobConf(conf);
      String host = InetAddress.getLocalHost().getHostName();
      jtConf.set("mapred.job.tracker", host + ":0");
      jtConf.set("mapred.job.tracker.http.address", "0.0.0.0:0");
      jtConf.set("mapred.jobtracker.taskScheduler",
          "org.apache.hadoop.mapred.MesosScheduler");
      // Start it
      JobTracker jt = JobTracker.startTracker(jtConf, jtId);
      // After startTracker, the JT has updated its internal variables to
      // include its real RPC and HTTP addresses; write them to our files
      writeTextFile(fs, jtPath, host + ":" + jt.getTrackerPort());
      fs.deleteOnExit(jtPath);
      writeTextFile(fs, webuiPath, host + ":" + jt.getInfoPort());
      fs.deleteOnExit(webuiPath);
      // Launch a daemon thread to touch the lock file and maintain our lease
      new LeaseRenewalThread(fs, lockPath, leaseTimeout, jt).start();
      // Run the JT
      jt.offerService();
    } catch (UnknownHostException e) {
      fatalError("failed to get hostname of local machine");
    } catch (InterruptedException e) {}
  }
  
  private static void writeTextFile(FileSystem fs, Path path, String string)
  throws IOException {
    Writer writer = new OutputStreamWriter(fs.create(path, true));
    writer.write(string);
    writer.close();
  }

  /**
   * Print a fatal error message and exit the program.
   * @param message Error to print
   */
  private static void fatalError(String message) {
    System.err.println("Fatal error: " + message);
    System.exit(1);
  }
}
