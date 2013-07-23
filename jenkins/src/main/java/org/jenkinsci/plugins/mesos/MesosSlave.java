package org.jenkinsci.plugins.mesos;

import hudson.Extension;
import hudson.model.Computer;
import hudson.model.Descriptor;
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

  @DataBoundConstructor
  public MesosSlave(
      String name, String numExecutors, String labelString, String idleTerminationMinutes)
      throws FormException, IOException
  {
    super(name,
          labelString, // node description.
          "jenkins",   // remoteFS.
          numExecutors,
          Mode.NORMAL,
          labelString, // Label.
          new MesosComputerLauncher(name),
          new MesosRetentionStrategy(idleTerminationMinutes),
          Collections.<NodeProperty<?>> emptyList());

    LOGGER.info("Constructing Mesos slave");
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

  @Override
  public DescriptorImpl getDescriptor() {
    return (DescriptorImpl) super.getDescriptor();
  }

  @Extension
  public static class DescriptorImpl extends SlaveDescriptor {
    @Override
    public String getDisplayName() {
      return "Mesos Slave";
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
