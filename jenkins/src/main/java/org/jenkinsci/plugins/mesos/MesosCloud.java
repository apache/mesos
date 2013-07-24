package org.jenkinsci.plugins.mesos;

import hudson.Extension;
import hudson.model.Computer;
import hudson.model.Descriptor;
import hudson.model.Descriptor.FormException;
import hudson.model.Hudson;
import hudson.model.Label;
import hudson.model.Node;
import hudson.model.Node.Mode;
import hudson.slaves.Cloud;
import hudson.slaves.NodeProperty;
import hudson.slaves.NodeProvisioner.PlannedNode;
import hudson.util.FormValidation;

import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.Callable;
import java.util.logging.Level;
import java.util.logging.Logger;

import javax.servlet.ServletException;

import jenkins.model.Jenkins;
import net.sf.json.JSONObject;

import org.kohsuke.stapler.DataBoundConstructor;
import org.kohsuke.stapler.QueryParameter;
import org.kohsuke.stapler.StaplerRequest;
import org.kohsuke.stapler.StaplerResponse;

public class MesosCloud extends Cloud {

  private String master;
  private String description;

  // TODO(vinod): Let the default values in MesosCloud/confg.jelly be inherited from here.
  private int slaveCpus = 1;
  private int slaveMem = 512; // MB.
  private int executorCpus = 1;
  private int executorMem = 128; // MB.
  private int idleTerminationMinutes = 3;

  private final String labelString = "mesos";

  private static String staticMaster;

  private static final Logger LOGGER = Logger.getLogger(MesosCloud.class.getName());

  @DataBoundConstructor
  public MesosCloud(String master, String description, int slaveCpus,
      int slaveMem, int executorCpus, int executorMem, int idleTerminationMinutes)
          throws NumberFormatException {
    super("MesosCloud");

    this.master = master;
    this.description = description;
    this.slaveCpus = slaveCpus;
    this.slaveMem = slaveMem;
    this.executorCpus = executorCpus;
    this.executorMem = executorMem;
    this.idleTerminationMinutes = idleTerminationMinutes;

    // Restart the scheduler if the master has changed or a scheduler is not up.
    if (!master.equals(staticMaster) || !Mesos.getInstance().isSchedulerRunning()) {
      if (!master.equals(staticMaster)) {
        LOGGER.info("Mesos master changed, restarting the scheduler");
      } else {
        LOGGER.info("Scheduler was down, restarting the scheduler");
      }

      Mesos.getInstance().stopScheduler();
      Mesos.getInstance().startScheduler(Jenkins.getInstance().getRootUrl(), master);
    } else {
      LOGGER.info("Mesos master has not changed, leaving the scheduler running");
    }

    staticMaster = master;
  }

  @Override
  public Collection<PlannedNode> provision(Label label, int excessWorkload) {
    List<PlannedNode> list = new ArrayList<PlannedNode>();
    final int numExecutors = excessWorkload;
    try {
      list.add(new PlannedNode(this.getDisplayName(), Computer.threadPoolForRemoting
          .submit(new Callable<Node>() {
            public Node call() throws Exception {
              // TODO(vinod): Instead of launching one slave with 'excessWorkload' executors,
              // launch multiple slaves with fewer executors per slave.
              MesosSlave s = doProvision(numExecutors);
              Hudson.getInstance().addNode(s);
              return s;
            }
          }), numExecutors));

    } catch (Exception e) {
      LOGGER.log(Level.WARNING, "Failed to create instances on Mesos", e);
      return Collections.emptyList();
    }

    return list;
  }

  private MesosSlave doProvision(int numExecutors) throws Descriptor.FormException, IOException {
    String name = "mesos-jenkins-" + UUID.randomUUID().toString();
    return new MesosSlave(name, numExecutors, labelString, slaveCpus, slaveMem,
        executorCpus, executorMem, idleTerminationMinutes);
  }

  @Override
  public boolean canProvision(Label label) {
    // Provisioning is simply creating a task for a jenkins slave.
    // Therefore, we can always provision as long as the label
    // matches "mesos".
    // TODO(vinod): The framework may not have the resources necessary
    // to start a task when it comes time to launch the slave.
    return label.matches(Label.parse(labelString));
  }

  public String getMaster() {
    return this.master;
  }

  public void setMaster(String master) {
    this.master = master;
  }

  public String getDescription() {
    return description;
  }

  public void setDescription(String description) {
    this.description = description;
  }

  @Override
  public DescriptorImpl getDescriptor() {
    return (DescriptorImpl) super.getDescriptor();
  }

  public static MesosCloud get() {
    return Hudson.getInstance().clouds.get(MesosCloud.class);
  }

  @Extension
  public static class DescriptorImpl extends Descriptor<Cloud> {
    private String master;
    private String description;

    @Override
    public String getDisplayName() {
      return "Mesos Cloud";
    }

    @Override
    public boolean configure(StaplerRequest request, JSONObject object) throws FormException {
      master = object.getString("master");
      description = object.getString("description");
      save();
      return super.configure(request, object);
    }

    /**
     * Test connection from configuration page.
     */
    public FormValidation doTestConnection(@QueryParameter String master)
        throws IOException, ServletException {
      master = master.trim();

      if (master.equals("local")) {
        return FormValidation.warning("'local' creates a local mesos cluster");
      }

      if (master.startsWith("zk://")) {
        return FormValidation.warning("Zookeeper paths can be used, but the connection cannot be " +
            "tested prior to saving this page.");
      }

      if (master.startsWith("http://")) {
        return FormValidation.error("Please omit 'http://'.");
      }

      try {
        // URL requires the protocol to be explicitly specified.
        HttpURLConnection urlConn =
          (HttpURLConnection) new URL("http://" + master).openConnection();
        urlConn.connect();
        int code = urlConn.getResponseCode();
        urlConn.disconnect();

        if (code == 200) {
          return FormValidation.ok("Connected to Mesos successfully");
        } else {
          return FormValidation.error("Status returned from url was " + code);
        }
      } catch (IOException e) {
        LOGGER.log(Level.WARNING, "Failed to connect to Mesos " + master, e);
        return FormValidation.error(e.getMessage());
      }
    }
  }

}
