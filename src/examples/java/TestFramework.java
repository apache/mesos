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

import org.apache.mesos.*;
import org.apache.mesos.Protos.*;


public class TestFramework {
  static class TestScheduler implements Scheduler {
    int launchedTasks = 0;
    int finishedTasks = 0;
    final int totalTasks;

    public TestScheduler() {
      this(5);
    }

    public TestScheduler(int numTasks) {
      totalTasks = numTasks;
    }

    @Override
    public void registered(SchedulerDriver driver, FrameworkID frameworkId) {
      System.out.println("Registered! ID = " + frameworkId.getValue());
    }

    @Override
    public void resourceOffers(SchedulerDriver driver,
                               List<Offer> offers) {
      for (Offer offer : offers) {
        List<TaskInfo> tasks = new ArrayList<TaskInfo>();
        if (launchedTasks < totalTasks) {
          TaskID taskId = TaskID.newBuilder()
            .setValue(Integer.toString(launchedTasks++)).build();

          System.out.println("Launching task " + taskId.getValue());

          TaskInfo task = TaskInfo.newBuilder()
            .setName("task " + taskId.getValue())
            .setTaskId(taskId)
            .setSlaveId(offer.getSlaveId())
            .addResources(Resource.newBuilder()
                          .setName("cpus")
                          .setType(Value.Type.SCALAR)
                          .setScalar(Value.Scalar.newBuilder()
                                     .setValue(1)
                                     .build())
                          .build())
            .addResources(Resource.newBuilder()
                          .setName("mem")
                          .setType(Value.Type.SCALAR)
                          .setScalar(Value.Scalar.newBuilder()
                                     .setValue(128)
                                     .build())
                          .build())
            .build();
          tasks.add(task);
        }
        Filters filters = Filters.newBuilder().setRefuseSeconds(1).build();
        driver.launchTasks(offer.getId(), tasks, filters);
      }
    }

    @Override
    public void offerRescinded(SchedulerDriver driver, OfferID offerId) {}

    @Override
    public void statusUpdate(SchedulerDriver driver, TaskStatus status) {
      System.out.println("Status update: task " + status.getTaskId().getValue() +
                         " is in state " + status.getState());
      if (status.getState() == TaskState.TASK_FINISHED) {
        finishedTasks++;
        System.out.println("Finished tasks: " + finishedTasks);
        if (finishedTasks == totalTasks) {
          driver.stop();
        }
      }
    }

    @Override
    public void frameworkMessage(SchedulerDriver driver, SlaveID slaveId, ExecutorID executorId, byte[] data) {}

    @Override
    public void slaveLost(SchedulerDriver driver, SlaveID slaveId) {}

    @Override
    public void error(SchedulerDriver driver, int code, String message) {
      System.out.println("Error: " + message);
    }
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

    ExecutorInfo executorInfo = ExecutorInfo.newBuilder()
      .setExecutorId(ExecutorID.newBuilder().setValue("default").build())
      .setCommand(CommandInfo.newBuilder().setUri(uri).setValue(uri).build())
      .build();

    MesosSchedulerDriver driver;

    if (args.length == 1) {
      driver = new MesosSchedulerDriver(
          new TestScheduler(),
          "Java test framework",
          executorInfo,
          args[0]);
    } else {
      driver = new MesosSchedulerDriver(
          new TestScheduler(Integer.parseInt(args[1])),
          "Java test framework",
          executorInfo,
          args[0]);
    }

    System.exit(driver.run() == Status.DRIVER_STOPPED ? 0 : 1);
  }
}
