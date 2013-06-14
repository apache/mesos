package org.jenkinsci.plugins.mesos;

import hudson.model.Computer;
import hudson.model.Descriptor.FormException;
import hudson.model.Hudson;
import hudson.model.Slave;
import hudson.slaves.NodeProperty;
import hudson.slaves.ComputerLauncher;
import hudson.slaves.RetentionStrategy;

import java.io.IOException;
import java.util.Collections;
import java.util.List;
import java.util.logging.Level;
import java.util.logging.Logger;

import org.kohsuke.stapler.DataBoundConstructor;

public class MesosSlave extends Slave {

  private static final Logger LOGGER = Logger.getLogger(MesosSlave.class
      .getName());

  private final String idleTerminationMinutes;
  private final String limitedBuildCount;
  private final String clusterId;

  @DataBoundConstructor
  public MesosSlave(String name, String description, String remoteFS,
      int numExecutors, Mode mode, String labelString,
      List<? extends NodeProperty<?>> nodeProperties,
      String idleTerminationMinutes, String limitedBuildCount)
      throws FormException, IOException {

    this(name, description, remoteFS, numExecutors, Mode.NORMAL, labelString,
        new MesosComputerLauncher(name), null, Collections
            .<NodeProperty<?>> emptyList(), idleTerminationMinutes,
        limitedBuildCount);
  }

  public MesosSlave(String name, String nodeDescription, String remoteFS,
      int numExecutors, Mode mode, String labelString,
      ComputerLauncher launcher,
      RetentionStrategy<MesosComputer> retentionStrategy,
      List<? extends NodeProperty<?>> nodeProperties,
      String idleTerminationMinutes, String limitedBuildCount)
      throws FormException, IOException {

    super(name, nodeDescription, remoteFS, numExecutors, Mode.NORMAL,
        labelString, launcher, new MesosRetentionStrategy(
            idleTerminationMinutes), Collections.<NodeProperty<?>> emptyList());

    LOGGER.info("Constructing Mesos slave");

    this.idleTerminationMinutes = idleTerminationMinutes;
    this.limitedBuildCount = limitedBuildCount;
    this.clusterId = null;
  }

  public void terminate() {
    LOGGER.info("Terminating slave " + getNodeName());
    try {
      // Remove the node from hudson.
      Hudson.getInstance().removeNode(this);

      ComputerLauncher launcher = getLauncher();

      // If this is a mesos computer launcher, terminate the launcher.
      if (launcher instanceof MesosComputerLauncher) {
        ((MesosComputerLauncher) launcher).terminate();
      }
    } catch (IOException e) {
      LOGGER.log(Level.WARNING, "Failed to terminate Mesos instance: "
          + getInstanceId(), e);
    }
  }

  private String getInstanceId() {
    return getNodeName();
  }

  public void idleTimeout() {
    LOGGER.info("Mesos instance idle time expired: " + getInstanceId()
        + ", terminate now");
    terminate();
  }

  @Override
  public Computer createComputer() {
    return new MesosComputer(this);
  }
}
