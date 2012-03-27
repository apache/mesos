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
 * Concrete implementation of a SchedulerDriver that connects a
 * Scheduler with a Mesos master. The MesosSchedulerDriver is
 * thread-safe.
 *
 * Note that scheduler failover is supported in Mesos. After a
 * scheduler is registered with Mesos it may failover (to a new
 * process on the same machine or across multiple machines) by
 * creating a new driver with the ID given to it in {@link
 * Scheduler#registered}.
 *
 * The driver is responsible for invoking the Scheduler callbacks as
 * it communicates with the Mesos master.
 *
 * Note that blocking on the MesosSchedulerDriver (e.g., via {@link
 * #join}) doesn't affect the scheduler callbacks in anyway because
 * they are handled by a different thread.
 *
 * See src/examples/java/TestFramework.java for an example of using
 * the MesosSchedulerDriver.
 */
public class MesosSchedulerDriver implements SchedulerDriver {
  static {
    MesosNativeLibrary.load();
  }

  /**
   * Creates a new scheduler driver that connects to a Mesos master
   * through the specified URL. Optionally providing an existing
   * framework ID can be used to failover a framework.
   *
   * Any Mesos configuration options are read from environment
   * variables, as well as any configuration files found through the
   * environment variables.
   */
  public MesosSchedulerDriver(Scheduler sched,
                              String frameworkName,
                              String url,
                              FrameworkID frameworkId) {
    if (sched == null) {
      throw new NullPointerException("Not expecting a null Scheduler");
    }

    if (frameworkName == null) {
      throw new NullPointerException("Not expecting a null framework name");
    }

    if (url == null) {
      throw new NullPointerException("Not expecting a null URL");
    }

    if (frameworkId == null) {
      throw new NullPointerException("Not expecting a null FrameworkID");
    }

    this.sched = sched;
    this.frameworkName = frameworkName;
    this.url = url;
    this.frameworkId = frameworkId;
    initialize();
  }

  /**
   * Creates a new scheduler driver. See above for details.
   */
  public MesosSchedulerDriver(Scheduler sched,
                              String frameworkName,
                              String url) {
    this(sched,
         frameworkName,
         url,
         FrameworkID.newBuilder().setValue("").build());
  }

  /**
   * See SchedulerDriver for descriptions of these.
   */
  public native Status start();
  public native Status stop(boolean failover);
  public Status stop() {
    return stop(false);
  }
  public native Status abort();
  public native Status join();

  public Status run() {
    Status status = start();
    return status != Status.DRIVER_RUNNING ? status : join();
  }

  public native Status requestResources(Collection<Request> requests);

  public Status launchTasks(OfferID offerId,
                            Collection<TaskInfo> tasks) {
    return launchTasks(offerId, tasks, Filters.newBuilder().build());
  }

  public native Status launchTasks(OfferID offerId,
                                   Collection<TaskInfo> tasks,
                                   Filters filters);

  public native Status killTask(TaskID taskId);

  public Status declineOffer(OfferID offerId) {
    return declineOffer(offerId, Filters.newBuilder().build());
  }

  public native Status declineOffer(OfferID offerId, Filters filters);

  public native Status reviveOffers();

  public native Status sendFrameworkMessage(ExecutorID executorId,
                                            SlaveID slaveId,
                                            byte[] data);

  protected native void initialize();
  protected native void finalize();

  private final Scheduler sched;
  private final String url;
  private final FrameworkID frameworkId;
  private final String frameworkName;

  private long __sched;
  private long __driver;
}
