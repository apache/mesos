package org.jenkinsci.plugins.mesos;

import static hudson.util.TimeUnit2.MINUTES;

import java.util.logging.Logger;

import org.kohsuke.stapler.DataBoundConstructor;

import hudson.model.Descriptor;
import hudson.slaves.RetentionStrategy;
import hudson.util.TimeUnit2;

/**
 * This is inspired by {@link hudson.slaves.CloudRetentionStrategy}.
 */
public class MesosRetentionStrategy extends RetentionStrategy<MesosComputer> {

  /**
   * Number of minutes of idleness before an instance should be terminated. A
   * value of zero indicates that the instance should never be automatically
   * terminated.
   */
  public final int idleTerminationMinutes;

  // Since a Mesos task is fast to start/stop we use a default value of 1 min.
  private final int IDLE_TERMINATION_MINUTES = 1;

  private static final Logger LOGGER = Logger
      .getLogger(MesosRetentionStrategy.class.getName());

  public MesosRetentionStrategy(String idleTerminationMinutes) {
    int value = IDLE_TERMINATION_MINUTES;
    if (idleTerminationMinutes != null && idleTerminationMinutes.trim() != "") {
      try {
        value = Integer.parseInt(idleTerminationMinutes);
      } catch (NumberFormatException nfe) {
        LOGGER.info("Malformed idleTermination value: " + idleTerminationMinutes);
      }
    }
    this.idleTerminationMinutes = value;
  }

  @Override
  public synchronized long check(MesosComputer c) {
    if (c.getNode() == null) {
      return 1;
    }

    // If we just launched this computer, check back after 1 min.
    // NOTE: 'c.getConnectTime()' refers to when the Jenkins slave was launched.
    if ((System.currentTimeMillis() - c.getConnectTime()) <
        MINUTES.toMillis(idleTerminationMinutes)) {
      return 1;
    }

    // If the computer is offline, terminate it.
    if (c.isOffline()) {
      LOGGER.info("Disconnecting offline computer " + c.getName());
      c.getNode().terminate();
      return 1;
    }

    // Terminate the computer if it is idle for longer than
    // 'idleTerminationMinutes'.
    if (c.isIdle()) {
      final long idleMilliseconds =
          System.currentTimeMillis() - c.getIdleStartMilliseconds();

      if (idleMilliseconds > MINUTES.toMillis(idleTerminationMinutes)) {
        LOGGER.info("Disconnecting idle computer " + c.getName());
        c.getNode().terminate();
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
