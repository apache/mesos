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
  private String labelString = "mesos";
  private static String staticMaster;

  private static final Logger LOGGER = Logger.getLogger(MesosCloud.class.getName());

  @DataBoundConstructor
  public MesosCloud(String master, String description) {
    super("MesosCloud");

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

    this.master = master;
    this.description = description;

    staticMaster = this.master;
  }

  @Override
  public Collection<PlannedNode> provision(Label label, int excessWorkload) {
    final int numExecutors = 1;
    List<PlannedNode> list = new ArrayList<PlannedNode>();

    try {
      list.add(new PlannedNode(this.getDisplayName(), Computer.threadPoolForRemoting
          .submit(new Callable<Node>() {
            public Node call() throws Exception {
              // TODO: record the output somewhere
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

  public void doProvision(StaplerRequest req, StaplerResponse rsp) throws ServletException,
      IOException, FormException {
    checkPermission(PROVISION);
    LOGGER.log(Level.INFO, "Create a new node by user request");

    try {
      MesosSlave node = doProvision(1);
      Hudson.getInstance().addNode(node);

      rsp.sendRedirect2(req.getContextPath() + "/computer/" + node.getNodeName());
    } catch (Exception e) {
      sendError(e.getMessage(), req, rsp);
    }
  }

  private MesosSlave doProvision(int numExecutors) throws Descriptor.FormException, IOException {
    String name = "mesos-jenkins-" + UUID.randomUUID().toString();
    String nodeDescription = labelString;
    String remoteFS = "jenkins";
    Mode mode = Mode.NORMAL;
    List<? extends NodeProperty<?>> nodeProperties = null;

    return new MesosSlave(name, nodeDescription, remoteFS, numExecutors, mode, labelString,
        nodeProperties, "1", "0");
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
    public boolean configure(StaplerRequest req, JSONObject o) throws FormException {
      master = o.getString("master");
      description = o.getString("description");
      save();
      return super.configure(req, o);
    }

    /**
     * Test connection from configuration page.
     */
    public FormValidation doTestConnection(@QueryParameter String master) throws IOException,
        ServletException {
      master = master.trim();

      if (master.equals("local")) {
        return FormValidation.warning("'local' triggers a local mesos cluster");
      }

      if (master.startsWith("zk://")) {
        return FormValidation.warning("Zookeeper paths can be used, but the connection cannot be " +
            "tested prior to saving this page.");
      }

      try {
        HttpURLConnection urlConn = (HttpURLConnection) new URL(master).openConnection();
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
