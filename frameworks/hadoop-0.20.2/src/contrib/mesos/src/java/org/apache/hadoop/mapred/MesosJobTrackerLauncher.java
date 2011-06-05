package org.apache.hadoop.mapred;

import java.io.BufferedReader;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.Random;

import org.apache.hadoop.fs.FSDataInputStream;
import org.apache.hadoop.fs.FSDataOutputStream;
import org.apache.hadoop.fs.FileStatus;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.util.Shell;

/**
 * Locates a running JobTracker with a given tag, or launches a new one
 * using the mesos.launcher.* Configuration properties.
 */
public class MesosJobTrackerLauncher {
  public static void main(String[] args) throws IOException {
    if (args.length != 1) {
      System.err.println("Usage: MesosJobTrackerLauncher <tag>");
      System.exit(1);
    }
    String tag = args[0];
    
    JobConf conf = new JobConf();
    FileSystem fs = FileSystem.get(conf);

    // Read configuration options
    String launcherDir = conf.get("mesos.launcher.dir", "/user/mesos/launcher");
    long leaseTimeout = conf.getLong("mesos.launcher.lease.timeout", 30000);
    String[] jtHosts = conf.getStrings("mesos.launcher.jobtracker.hosts");
    if (jtHosts == null) {
      fatalError("mesos.launcher.jobtracker.hosts is not set");
    }
    
    // Check that HADOOP_HOME is set
    String hadoopHome = System.getenv("HADOOP_HOME");
    if (hadoopHome == null) {
      fatalError("HADOOP_HOME environment variable must be set");
    }
    
    // Create the lock directory if it does not exist
    Path launcherDirPath = new Path(launcherDir);
    try {
      FileStatus status = fs.getFileStatus(launcherDirPath);
      if (!status.isDir()) {
        fatalError(launcherDir + " exists, but is not a directory");
      }
    } catch (FileNotFoundException e) {
      if (!fs.mkdirs(launcherDirPath)) {
        fatalError("failed to create " + launcherDir);
      }
    }
    
    // Try to connect to, or launch, a JobTracker for our given tag
    Path lockPath = new Path(launcherDirPath, tag + ".lock");
    Path jtPath = new Path(launcherDirPath, tag + ".jobtracker");
    while (true) {
      try {
        FileStatus stat = fs.getFileStatus(lockPath);
        // If getFileStatus succeeds, the lock file exists; check if it's valid
        if (stat.getAccessTime() + leaseTimeout < System.currentTimeMillis()) {
          // Lease expired; delete the file so that we can attempt to create it
          // on the next iteration of the while loop, but sleep a bit before
          // continuing in case other launchers are trying to delete the file.
          fs.delete(lockPath, false);
          try {
            Thread.sleep(2000);
          } catch (InterruptedException e) {}
        } else {
          // Lease is valid; test and return the JobTracker in the jtPath file
          try {
            FSDataInputStream in = fs.open(jtPath);
            BufferedReader br = new BufferedReader(new InputStreamReader(in));
            String jtAddress = br.readLine().trim();
            in.close();
            // Try to connect to the JT listed in jtAddress
            JobConf clientConf = new JobConf(conf);
            clientConf.set("mapred.job.tracker", jtAddress);
            JobClient client = new JobClient();
            client.init(clientConf);
            client.close();
            // Everything worked! Print the JobTracker URL and exit
            System.out.println(jtAddress);
            System.exit(0);
          } catch (Exception e) {
            // Failed to read the jobTracker file or to connect to the 
            // JobTracker listed there. This could mean that the JT is still
            // starting up, or that it failed. Retry in two seconds.
            try {
              Thread.sleep(2000);
            } catch (InterruptedException e2) {}
          }
        }
      } catch (FileNotFoundException fnf) {
        // Lock file does not exist; try creating it ourselves
        try {
          FSDataOutputStream out = fs.create(lockPath, false);
          out.close();
          fs.setTimes(lockPath, -1, System.currentTimeMillis());
          // If we got here, we created the lock file successfully; go ahead
          // and launch the JobTracker
          Random r = new Random();
          String host = jtHosts[r.nextInt(jtHosts.length)];
          Shell.execCommand(
              "ssh", host, hadoopHome + "/bin/mesos-jobtracker-runner", tag);
          // Sleep a bit to give the JT a chance to start before we test it
          try {
            Thread.sleep(5000);
          } catch (InterruptedException e) {}
        } catch (IOException e) {}
      }
    }
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
