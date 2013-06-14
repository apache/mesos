package org.jenkinsci.plugins.mesos;

import hudson.model.Slave;
import hudson.slaves.SlaveComputer;

import java.io.IOException;

import org.kohsuke.stapler.HttpRedirect;
import org.kohsuke.stapler.HttpResponse;

public class MesosComputer extends SlaveComputer {
  public MesosComputer(Slave slave) {
    super(slave);
  }

  @Override
  public MesosSlave getNode() {
    return (MesosSlave) super.getNode();
  }

  @Override
  public HttpResponse doDoDelete() throws IOException {
    checkPermission(DELETE);
    if (getNode() != null)
      getNode().terminate();
    return new HttpRedirect("..");
  }
}
