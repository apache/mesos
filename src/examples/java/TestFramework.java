/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import java.io.File;
import java.io.IOException;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import com.google.protobuf.ByteString;

import org.apache.mesos.*;
import org.apache.mesos.Protos.*;
import org.apache.mesos.scheduler.Protos.OfferConstraints;
import org.apache.mesos.scheduler.Protos.AttributeConstraint;

public class TestFramework {
  static class TestScheduler implements Scheduler {
    public TestScheduler(boolean implicitAcknowledgements,
                         ExecutorInfo executor,
                         FrameworkInfo framework,
                         int totalTasks) {
      this.implicitAcknowledgements = implicitAcknowledgements;
      this.executor = executor;
      this.framework = framework;
      this.totalTasks = totalTasks;
    }

    @Override
    public void registered(SchedulerDriver driver,
                           FrameworkID frameworkId,
                           MasterInfo masterInfo) {
      System.out.println("Registered! ID = " + frameworkId.getValue());

      // Clear suppressed roles.
      FrameworkInfo.Builder builder = framework.toBuilder();
      builder.setId(frameworkId);
      driver.updateFramework(
          builder.build(),
          new ArrayList<String>(),
          OfferConstraints.getDefaultInstance());
    }

    @Override
    public void reregistered(SchedulerDriver driver, MasterInfo masterInfo) {}

    @Override
    public void disconnected(SchedulerDriver driver) {}

    @Override
    public void resourceOffers(SchedulerDriver driver,
                               List<Offer> offers) {
      double CPUS_PER_TASK = 1;
      double MEM_PER_TASK = 128;

      for (Offer offer : offers) {
        Offer.Operation.Launch.Builder launch = Offer.Operation.Launch.newBuilder();
        double offerCpus = 0;
        double offerMem = 0;
        for (Resource resource : offer.getResourcesList()) {
          if (resource.getName().equals("cpus")) {
            offerCpus += resource.getScalar().getValue();
          } else if (resource.getName().equals("mem")) {
            offerMem += resource.getScalar().getValue();
          }
        }

        System.out.println(
            "Received offer " + offer.getId().getValue() + " with cpus: " + offerCpus +
            " and mem: " + offerMem);

        double remainingCpus = offerCpus;
        double remainingMem = offerMem;
        while (launchedTasks < totalTasks &&
               remainingCpus >= CPUS_PER_TASK &&
               remainingMem >= MEM_PER_TASK) {
          TaskID taskId = TaskID.newBuilder()
            .setValue(Integer.toString(launchedTasks++)).build();

          System.out.println("Launching task " + taskId.getValue() +
                             " using offer " + offer.getId().getValue());

          TaskInfo task = TaskInfo.newBuilder()
            .setName("task " + taskId.getValue())
            .setTaskId(taskId)
            .setSlaveId(offer.getSlaveId())
            .addResources(Resource.newBuilder()
                          .setName("cpus")
                          .setType(Value.Type.SCALAR)
                          .setScalar(Value.Scalar.newBuilder().setValue(CPUS_PER_TASK)))
            .addResources(Resource.newBuilder()
                          .setName("mem")
                          .setType(Value.Type.SCALAR)
                          .setScalar(Value.Scalar.newBuilder().setValue(MEM_PER_TASK)))
            .setExecutor(ExecutorInfo.newBuilder(executor))
            .build();

          launch.addTaskInfos(TaskInfo.newBuilder(task));

          remainingCpus -= CPUS_PER_TASK;
          remainingMem -= MEM_PER_TASK;
        }

        // NOTE: We use the new API `acceptOffers` here to launch tasks. The
        // 'launchTasks' API will be deprecated.
        List<OfferID> offerIds = new ArrayList<OfferID>();
        offerIds.add(offer.getId());

        List<Offer.Operation> operations = new ArrayList<Offer.Operation>();

        Offer.Operation operation = Offer.Operation.newBuilder()
          .setType(Offer.Operation.Type.LAUNCH)
          .setLaunch(launch)
          .build();

        operations.add(operation);

        Filters filters = Filters.newBuilder().setRefuseSeconds(1).build();

        driver.acceptOffers(offerIds, operations, filters);
      }
    }

    @Override
    public void offerRescinded(SchedulerDriver driver, OfferID offerId) {}

    @Override
    public void statusUpdate(SchedulerDriver driver, TaskStatus status) {
      System.out.println("Status update: task " + status.getTaskId().getValue() +
                         " is in state " + status.getState().getValueDescriptor().getName());
      if (status.getState() == TaskState.TASK_FINISHED) {
        finishedTasks++;

        System.out.println("Finished tasks: " + finishedTasks);
        if (finishedTasks == totalTasks) {
          driver.stop();
        }
      }

      if (status.getState() == TaskState.TASK_LOST ||
          status.getState() == TaskState.TASK_KILLED ||
          status.getState() == TaskState.TASK_FAILED) {
        System.err.println("Aborting because task " + status.getTaskId().getValue() +
                           " is in unexpected state " +
                           status.getState().getValueDescriptor().getName() +
                           " with reason '" +
                           status.getReason().getValueDescriptor().getName() + "'" +
                           " from source '" +
                           status.getSource().getValueDescriptor().getName() + "'" +
                           " with message '" + status.getMessage() + "'");
        driver.abort();
      }

      if (!implicitAcknowledgements) {
        driver.acknowledgeStatusUpdate(status);
      }
    }

    @Override
    public void frameworkMessage(SchedulerDriver driver,
                                 ExecutorID executorId,
                                 SlaveID slaveId,
                                 byte[] data) {}

    @Override
    public void slaveLost(SchedulerDriver driver, SlaveID slaveId) {}

    @Override
    public void executorLost(SchedulerDriver driver,
                             ExecutorID executorId,
                             SlaveID slaveId,
                             int status) {}

    public void error(SchedulerDriver driver, String message) {
      System.out.println("Error: " + message);
    }

    private final boolean implicitAcknowledgements;
    private final ExecutorInfo executor;
    private final FrameworkInfo framework;
    private final int totalTasks;
    private int launchedTasks = 0;
    private int finishedTasks = 0;
  }

  private static void usage() {
    String name = TestFramework.class.getName();
    System.err.println("Usage: " + name + " master <tasks>");
  }

  public static void main(String[] args) throws Exception {
    if (args.length < 1 || args.length > 2) {
      usage();
      System.exit(1);
    }

    String uri = new File("./test-executor").getCanonicalPath();

    ExecutorInfo executor = ExecutorInfo.newBuilder()
      .setExecutorId(ExecutorID.newBuilder().setValue("default"))
      .setCommand(CommandInfo.newBuilder().setValue(uri))
      .setName("Test Executor (Java)")
      .setSource("java_test")
      .build();

    String role = "*";

    FrameworkInfo.Builder frameworkBuilder = FrameworkInfo.newBuilder()
        .setUser("") // Have Mesos fill in the current user.
        .setName("Test Framework (Java)")
        .setCheckpoint(true)
        .setRole(role);

    boolean implicitAcknowledgements = true;

    if (System.getenv("MESOS_EXPLICIT_ACKNOWLEDGEMENTS") != null) {
      System.out.println("Enabling explicit acknowledgements for status updates");
      implicitAcknowledgements = false;
    }

    int totalTasks = args.length == 1 ? 5: Integer.parseInt(args[1]);
    Scheduler scheduler = null;

    // The framework subscribes with all roles suppressed
    // to test unsuppression via 'updateFramework()'
    List<String> suppressedRoles = new ArrayList<String>();
    suppressedRoles.add(role);

    MesosSchedulerDriver driver = null;
    if (System.getenv("MESOS_EXAMPLE_AUTHENTICATE") != null) {
      System.out.println("Enabling authentication for the framework");

      if (System.getenv("MESOS_EXAMPLE_PRINCIPAL") == null) {
        System.err.println("Expecting authentication principal in the environment");
        System.exit(1);
      }

      Credential.Builder credentialBuilder = Credential.newBuilder()
        .setPrincipal(System.getenv("MESOS_EXAMPLE_PRINCIPAL"));

      if (System.getenv("MESOS_EXAMPLE_SECRET") != null) {
          credentialBuilder.setSecret(System.getenv("MESOS_EXAMPLE_SECRET"));
      }

      frameworkBuilder.setPrincipal(System.getenv("MESOS_EXAMPLE_PRINCIPAL"));
      FrameworkInfo framework = frameworkBuilder.build();
      scheduler = new TestScheduler(
          implicitAcknowledgements, executor, framework, totalTasks);

      driver = new MesosSchedulerDriver(
          scheduler,
          framework,
          suppressedRoles,
          args[0],
          implicitAcknowledgements,
          credentialBuilder.build());
    } else {
      frameworkBuilder.setPrincipal("test-framework-java");
      FrameworkInfo framework = frameworkBuilder.build();
      scheduler = new TestScheduler(
          implicitAcknowledgements, executor, framework, totalTasks);

      driver = new MesosSchedulerDriver(scheduler, framework, suppressedRoles,
                                        args[0], implicitAcknowledgements);
    }

    int status = driver.run() == Status.DRIVER_STOPPED ? 0 : 1;

    // Ensure that the driver process terminates.
    driver.stop();

    // For this test to pass reliably on some platforms, this sleep is
    // required to ensure that the SchedulerDriver teardown is complete
    // before the JVM starts running native object destructors after
    // System.exit() is called. 500ms proved successful in test runs,
    // but on a heavily loaded machine it might not.
    // TODO(greg): Ideally, we would inspect the status of the driver
    // and its associated tasks via the Java API and wait until their
    // teardown is complete to exit.
    Thread.sleep(500);

    System.exit(status);
  }
}
