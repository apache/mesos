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
import java.util.Timer;
import java.util.TimerTask;

import java.util.concurrent.locks.*;

import org.apache.mesos.v1.*;

import org.apache.mesos.v1.Protos.*;

import org.apache.mesos.v1.scheduler.Mesos;
import org.apache.mesos.v1.scheduler.Scheduler;
import org.apache.mesos.v1.scheduler.V0Mesos;
import org.apache.mesos.v1.scheduler.V1Mesos;

import org.apache.mesos.v1.scheduler.Protos.Call;
import org.apache.mesos.v1.scheduler.Protos.Event;

public class V1TestFramework {
  static class TestScheduler implements Scheduler {

    public TestScheduler(
        String master,
        FrameworkInfo framework,
        ExecutorInfo executor) {
      this(master, framework, executor, 5);
    }

    public TestScheduler(
        String master,
        FrameworkInfo framework,
        ExecutorInfo executor,
        int totalTasks) {
      this.framework = framework;
      this.executor = executor;
      this.totalTasks = totalTasks;
      this.state = State.DISCONNECTED;
    }

    // TODO(anand): Synchronize on `state` instead.
    @Override
    public synchronized void connected(final Mesos mesos) {
      System.out.println("Connected");

      state = State.CONNECTED;

      retryTimer = new Timer();
      retryTimer.schedule(new TimerTask() {
        @Override
        public void run() {
          doReliableRegistration(mesos);
        }
      }, 0, 1000); // Repeat every 1 second
    }

    @Override
    public synchronized void disconnected(Mesos mesos) {
      System.out.println("Disconnected");

      state = state.DISCONNECTED;
      cancelRetryTimer();
    }

    @Override
    public synchronized void received(Mesos mesos, Event event) {
      switch (event.getType()) {
        case SUBSCRIBED: {
          frameworkId = event.getSubscribed().getFrameworkId();
          state = State.SUBSCRIBED;

          System.out.println(
              "Subscribed with ID " + frameworkId + " to master " +
              event.getSubscribed().getMasterInfo().getId());
          break;
        }

        case OFFERS: {
          System.out.println("Received an OFFERS event");

          offers(mesos, event.getOffers().getOffersList());
          break;
        }

        case RESCIND: {
          System.out.println("Received an RESCIND event");
          break;
        }

        case UPDATE: {
          System.out.println("Received an UPDATE event");

          update(mesos, event.getUpdate().getStatus());
          break;
        }

        case MESSAGE: {
          System.out.println("Received a MESSAGE event");
          break;
        }

        case FAILURE: {
          System.out.println("Received a FAILURE event");
          break;
        }

        case ERROR: {
          System.out.println("Received an ERROR event");
          System.exit(1);
        }

        case HEARTBEAT: {
          // TODO(anand): Force reconnection with the master upon lack
          // of heartbeats.
          System.out.println("Received a HEARTBEAT event");
          break;
        }

        case UNKNOWN: {
          System.out.println("Received an UNKNOWN event");
          break;
        }
      }
    }

    public synchronized void doReliableRegistration(Mesos mesos) {
      if (state == State.SUBSCRIBED || state == State.DISCONNECTED) {
        cancelRetryTimer();
        return;
      }

      Call.Builder callBuilder = Call.newBuilder()
          .setType(Call.Type.SUBSCRIBE)
          .setSubscribe(Call.Subscribe.newBuilder()
            .setFrameworkInfo(framework)
            .build());

      mesos.send(callBuilder.build());
    }

    private void cancelRetryTimer() {
      // Cancel previously active timer (if one exists).
      if (retryTimer != null) {
        retryTimer.cancel();
        retryTimer.purge();
      }

      retryTimer = null;
    }

    public void offers(Mesos mesos, List<Offer> offers) {
      double CPUS_PER_TASK = 1;
      double MEM_PER_TASK = 128;

      for (Offer offer : offers) {
        Offer.Operation.Launch.Builder launch =
          Offer.Operation.Launch.newBuilder();

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
            "Received offer " + offer.getId().getValue() + " with cpus: " +
            offerCpus + " and mem: " + offerMem);

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
            .setAgentId(offer.getAgentId())
            .addResources(Resource.newBuilder()
                          .setName("cpus")
                          .setType(Value.Type.SCALAR)
                          .setScalar(Value.Scalar.newBuilder()
                            .setValue(CPUS_PER_TASK)
                            .build()))
            .addResources(Resource.newBuilder()
                          .setName("mem")
                          .setType(Value.Type.SCALAR)
                          .setScalar(Value.Scalar.newBuilder()
                            .setValue(MEM_PER_TASK)
                            .build()))
            .setExecutor(ExecutorInfo.newBuilder(executor))
            .build();

          launch.addTaskInfos(TaskInfo.newBuilder(task));

          remainingCpus -= CPUS_PER_TASK;
          remainingMem -= MEM_PER_TASK;
        }

        mesos.send(Call.newBuilder()
          .setType(Call.Type.ACCEPT)
          .setFrameworkId(frameworkId)
          .setAccept(Call.Accept.newBuilder()
            .addOfferIds(offer.getId())
            .addOperations(Offer.Operation.newBuilder()
              .setType(Offer.Operation.Type.LAUNCH)
              .setLaunch(launch)
              .build())
            .setFilters(Filters.newBuilder()
              .setRefuseSeconds(1)
              .build()))
          .build());
      }
    }

    public void update(Mesos mesos, TaskStatus status) {
      System.out.println(
          "Status update: task " + status.getTaskId().getValue() +
          " is in state " + status.getState().getValueDescriptor().getName());

      if (status.getState() == TaskState.TASK_FINISHED) {
        finishedTasks++;
        System.out.println("Finished tasks: " + finishedTasks);
        if (finishedTasks == totalTasks) {
          lock.lock();
          try {
            finished = true;
            finishedCondtion.signal();
          } finally {
            lock.unlock();
          }
        }
      }

      if (status.getState() == TaskState.TASK_LOST ||
          status.getState() == TaskState.TASK_KILLED ||
          status.getState() == TaskState.TASK_FAILED) {
        System.err.println(
            "Aborting because task " + status.getTaskId().getValue() +
            " is in unexpected state " +
            status.getState().getValueDescriptor().getName() +
            " with reason '" +
            status.getReason().getValueDescriptor().getName() + "'" +
            " from source '" +
            status.getSource().getValueDescriptor().getName() + "'" +
            " with message '" + status.getMessage() + "'");

        System.exit(1);
      }

      mesos.send(Call.newBuilder()
        .setType(Call.Type.ACKNOWLEDGE)
        .setFrameworkId(frameworkId)
        .setAcknowledge(Call.Acknowledge.newBuilder()
          .setAgentId(status.getAgentId())
          .setTaskId(status.getTaskId())
          .setUuid(status.getUuid())
          .build())
        .build());
    }

    private enum State {
      DISCONNECTED,
      CONNECTED,
      SUBSCRIBED
    }

    private FrameworkInfo framework;
    private FrameworkID frameworkId;
    private final ExecutorInfo executor;
    private final int totalTasks;
    private int launchedTasks = 0;
    private int finishedTasks = 0;
    private State state;
    private Timer retryTimer = null;
  }

  private static void usage() {
    String name = V1TestFramework.class.getName();
    System.err.println("Usage: " + name + " master version{0,1}");
  }

  public static void main(String[] args) throws Exception {
    if (args.length < 2 || args.length > 3) {
      usage();
      System.exit(1);
    }

    int version = Integer.parseInt(args[1]);
    if (version != 0 && version != 1) {
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

    FrameworkInfo.Builder frameworkBuilder = FrameworkInfo.newBuilder()
        .setUser(System.getProperty("user.name", "default-user"))
        .setName("V" + version + " Test Framework (Java)");

    Credential.Builder credentialBuilder = null;

    if (System.getenv("MESOS_EXAMPLE_PRINCIPAL") != null) {
      frameworkBuilder.setPrincipal(System.getenv("MESOS_EXAMPLE_PRINCIPAL"));

      if (System.getenv("MESOS_EXAMPLE_SECRET") != null) {
        credentialBuilder = Credential.newBuilder()
          .setPrincipal(System.getenv("MESOS_EXAMPLE_PRINCIPAL"))
          .setSecret(System.getenv("MESOS_EXAMPLE_SECRET"));
      }
    }

    Scheduler scheduler = args.length == 2
      ? new TestScheduler(args[0], frameworkBuilder.build(), executor)
      : new TestScheduler(args[0],
                          frameworkBuilder.build(),
                          executor,
                          Integer.parseInt(args[2]));

    Mesos mesos;
    if (credentialBuilder != null) {
      mesos = version == 1
                ? new V1Mesos(scheduler, args[0], credentialBuilder.build())
                : new V0Mesos(scheduler,
                              frameworkBuilder.build(),
                              args[0],
                              credentialBuilder.build());
    } else {
      mesos = version == 1
                ? new V1Mesos(scheduler, args[0])
                : new V0Mesos(scheduler, frameworkBuilder.build(), args[0]);
    }

    lock.lock();
    try {
      while (!finished) {
        finishedCondtion.await();
      }
    } finally {
      lock.unlock();
    }

    // NOTE: Copied from src/examples/java/TestFramework.java:
    // For this test to pass reliably on some platforms, this sleep is
    // required to ensure that the SchedulerDriver teardown is complete
    // before the JVM starts running native object destructors after
    // System.exit() is called. 500ms proved successful in test runs,
    // but on a heavily loaded machine it might not.

    // TODO(greg): Ideally, we would inspect the status of the driver
    // and its associated tasks via the Java API and wait until their
    // teardown is complete to exit.
    Thread.sleep(500);

    System.exit(0);
  }

  static boolean finished = false;
  final static Lock lock = new ReentrantLock();
  final static Condition finishedCondtion = lock.newCondition();
}
