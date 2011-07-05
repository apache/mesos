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

  public MesosSchedulerDriver(Scheduler sched, String url, FrameworkID frameworkId) {
    this.sched = sched;
    this.url = url;
    this.frameworkId = frameworkId;

    initialize();
  }

  public MesosSchedulerDriver(Scheduler sched, String url) {
    this(sched, url, FrameworkID.newBuilder().setValue("").build());
  }

  public native int start();
  public native int stop();
  public native int join();

  public int run() {
    int ret = start();
    return ret != 0 ? ret : join();
  }

  public native int sendFrameworkMessage(SlaveID slaveId, ExecutorID executorId, byte[] data);
  public native int killTask(TaskID taskId);
  public native int replyToOffer(OfferID offerId, Collection<TaskDescription> tasks, Map<String, String> params);

  public int replyToOffer(OfferID offerId, Collection<TaskDescription> tasks) {
    return replyToOffer(offerId, tasks, Collections.<String, String>emptyMap());
  }

  public native int reviveOffers();

  protected native void initialize();
  protected native void finalize();

  private final Scheduler sched;
  private final String url;
  private final FrameworkID frameworkId;

  private long __sched;
  private long __driver;
};
