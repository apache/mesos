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

package org.apache.mesos.v1.scheduler;

import org.apache.mesos.MesosNativeLibrary;

import org.apache.mesos.v1.Protos.Credential;
import org.apache.mesos.v1.Protos.FrameworkInfo;

import org.apache.mesos.v1.scheduler.Protos.Call;

/**
 * Concrete implementation of the Mesos interface that connects a Scheduler
 * with a Mesos master. This class is thread-safe.
 * <p>
 * This implementation uses the scheduler library (src/scheduler/scheduler.cpp)
 * based on the V1 Mesos Scheduler API. The library is responsible for
 * invoking the Scheduler callbacks as it communicates with the Mesos master.
 * <p>
 * <p>
 * Note that the scheduler library uses GLOG to do its own logging. GLOG flags
 * can be set via environment variables, prefixing the flag name with
 * "GLOG_", e.g., "GLOG_v=1". For Mesos specific logging flags see
 * src/logging/flags.hpp. Mesos flags can also be set via environment
 * variables, prefixing the flag name with "MESOS_", e.g., "MESOS_QUIET=1".
 * <p>
 * See src/examples/java/V1TestFramework.java for an example of using this.
 */
public class V1Mesos implements Mesos {
  static {
    MesosNativeLibrary.load();
  }

  public V1Mesos(Scheduler scheduler, String master) {
    this(scheduler, master, null);
  }

  public V1Mesos(Scheduler scheduler, String master, Credential credential) {
    if (scheduler == null) {
      throw new NullPointerException("Not expecting a null scheduler");
    }

    if (master == null) {
      throw new NullPointerException("Not expecting a null master");
    }

    this.scheduler = scheduler;
    this.master = master;
    this.credential = credential;

    initialize();
  }

  @Override
  public native void send(Call call);

  @Override
  public native void reconnect();

  protected native void initialize();
  protected native void finalize();

  private final Scheduler scheduler;
  private final String master;
  private final Credential credential;

  private long __mesos;
}
