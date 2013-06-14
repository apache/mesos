package org.jenkinsci.plugins.mesos;

import java.util.logging.Logger;

import org.kohsuke.stapler.DataBoundConstructor;

import hudson.model.Descriptor;
import hudson.slaves.RetentionStrategy;
import hudson.util.TimeUnit2;

/**
 * This is basically a copy of the EC2 plugin's retention strategy.
 * https://github.com/jenkinsci/ec2-plugin/blob/master/src/main/java/hudson
 * /plugins/ec2/EC2RetentionStrategy.java
 */
public class MesosRetentionStrategy extends RetentionStrategy<MesosComputer> {

  /**
   * Number of minutes of idleness before an instance should be terminated. A
   * value of zero indicates that the instance should never be automatically
   * terminated.
   */
  public final int idleTerminationMinutes;

  private static final Logger LOGGER = Logger
      .getLogger(MesosRetentionStrategy.class.getName());

  public static boolean disabled = Boolean
      .getBoolean(MesosRetentionStrategy.class.getName() + ".disabled");

  @DataBoundConstructor
  public MesosRetentionStrategy(String idleTerminationMinutes) {
    // Since a Mesos node is fast to start/stop we default this value to 3 mins.
    int value = 3;
    if (idleTerminationMinutes != null && idleTerminationMinutes.trim() != "") {
      try {
        value = Integer.parseInt(idleTerminationMinutes);
      } catch (NumberFormatException nfe) {
        LOGGER.info("Malformed default idleTermination value: "
            + idleTerminationMinutes);
      }
    }
    this.idleTerminationMinutes = value;
  }

  @Override
  public synchronized long check(MesosComputer c) {
    // If we've been told never to terminate, then we're done.
    if (idleTerminationMinutes == 0)
      return 1;

    final long idleMilliseconds1 = System.currentTimeMillis()
        - c.getIdleStartMilliseconds();

    System.out.println(c.getName() + " idle: " + idleMilliseconds1);

    if (c.isIdle() && c.isOnline() && !disabled) {
      final long idleMilliseconds = System.currentTimeMillis()
          - c.getIdleStartMilliseconds();

      if (idleMilliseconds > TimeUnit2.MINUTES.toMillis(idleTerminationMinutes)) {
        LOGGER.info("Idle timeout after " + idleTerminationMinutes + "mins: "
            + c.getName());
        c.getNode().idleTimeout();
      }
    }
    return 1;
  }

  /**
   * Try to connect to it ASAP to launch the slave agent.
   */
  @Override
  public void start(MesosComputer c) {
    c.connect(false);
  }

  /**
   * No registration since this retention strategy is used only for Mesos nodes
   * that we provision automatically.
   */
  public static class DescriptorImpl extends Descriptor<RetentionStrategy<?>> {
    @Override
    public String getDisplayName() {
      return "MESOS";
    }
  }
}
