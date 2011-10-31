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

package org.apache.mesos;

import org.apache.mesos.Protos.*;

import java.util.Collection;
import java.util.Collections;
import java.util.Map;


/**
 * Concrete implementation of SchedulerDriver that communicates with
 * a Mesos master.
 */
public class MesosSchedulerDriver implements SchedulerDriver {
  static {
    System.loadLibrary("mesos");
  }

  public MesosSchedulerDriver(Scheduler sched,
                              String frameworkName,
                              ExecutorInfo executorInfo,
                              String url,
                              FrameworkID frameworkId) {
    this.sched = sched;
    this.url = url;
    this.frameworkId = frameworkId;
    this.frameworkName = frameworkName;
    this.executorInfo = executorInfo;
    initialize();
  }

  public MesosSchedulerDriver(Scheduler sched, String frameworkName, ExecutorInfo executorInfo, String url) {
    this(sched, frameworkName, executorInfo, url, FrameworkID.newBuilder().setValue("").build());
  }

  public native Status start();
  public native Status stop(boolean failover);
  public Status stop() {
    return stop(false);
  }
  public native Status abort();
  public native Status join();

  public Status run() {
    Status ret = start();
    return ret != Status.OK ? ret : join();
  }

  public native Status requestResources(Collection<ResourceRequest> requests);

  public Status launchTasks(OfferID offerId, Collection<TaskDescription> tasks) {
    return launchTasks(offerId, tasks, Filters.newBuilder().build());
  }

  public native Status launchTasks(OfferID offerId, Collection<TaskDescription> tasks, Filters filters);

  public native Status killTask(TaskID taskId);

  public native Status reviveOffers();

  public native Status sendFrameworkMessage(SlaveID slaveId, ExecutorID executorId, byte[] data);

  protected native void initialize();
  protected native void finalize();

  private final Scheduler sched;
  private final String url;
  private final FrameworkID frameworkId;
  private final String frameworkName;
  private final ExecutorInfo executorInfo;

  private long __sched;
  private long __driver;
};
