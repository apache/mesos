package org.jenkinsci.plugins.mesos;

import org.kohsuke.stapler.DataBoundConstructor;

import hudson.Extension;
import hudson.model.Describable;
import hudson.model.Descriptor;
import hudson.model.Hudson;

public class TaskTemplate implements Describable<TaskTemplate> {
  public final String description;
  public final String label;
  public final String idleTerminationMinutes;
  public final String numExecutors;
  public final String executorMem;
  public final String slaveJarMem;

  @DataBoundConstructor
  public TaskTemplate(String description, String label, String idleTerminationMinutes, String numExecutors, String executorMem, String slaveJarMem) {
    this.description = description;
    this.label = label;
    this.idleTerminationMinutes = idleTerminationMinutes;
    this.numExecutors = numExecutors;
    this.executorMem = executorMem;
    this.slaveJarMem = slaveJarMem;
  }

  @Override
  public Descriptor<TaskTemplate> getDescriptor() {
    return Hudson.getInstance().getDescriptor(getClass());
  }

  @Extension
  public static final class DescriptorImpl extends Descriptor<TaskTemplate> {
    @Override public String getDisplayName() {
      return null;
    }
  }
}
