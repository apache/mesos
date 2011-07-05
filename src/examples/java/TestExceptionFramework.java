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

import java.util.List;

import org.apache.mesos.*;
import org.apache.mesos.Protos.*;


public class TestExceptionFramework {
  static class MyScheduler implements Scheduler {
    @Override
    public String getFrameworkName(SchedulerDriver driver) {
      throw new ArrayIndexOutOfBoundsException();
    }

    @Override
    public ExecutorInfo getExecutorInfo(SchedulerDriver driver) {
      try {
        File file = new File("./test_executor");
        return ExecutorInfo.newBuilder()
          .setExecutorId(ExecutorID.newBuilder().setValue("default").build())
          .setUri(file.getCanonicalPath())
          .build();
      } catch (Throwable t) {
        throw new RuntimeException(t);
      }
    }

    @Override
    public void registered(SchedulerDriver driver, FrameworkID frameworkId) {}

    @Override
    public void resourceOffer(SchedulerDriver driver,
                              OfferID offerId,
                              List<SlaveOffer> offers) {}

    @Override
    public void offerRescinded(SchedulerDriver driver, OfferID offerId) {}

    @Override
    public void statusUpdate(SchedulerDriver driver, TaskStatus status) {}

    @Override
    public void frameworkMessage(SchedulerDriver driver, SlaveID slaveId, ExecutorID executorId, byte[] data) {}

    @Override
    public void slaveLost(SchedulerDriver driver, SlaveID slaveId) {}

    @Override
    public void error(SchedulerDriver driver, int code, String message) {}
  }

  public static void main(String[] args) throws Exception {
    new MesosSchedulerDriver(new MyScheduler(), args[0]).run();
  }
}
