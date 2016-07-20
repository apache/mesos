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
import org.apache.mesos.v1.scheduler.Protos.Event;

/**
 * This implementation acts as an adapter from the v0 (driver + scheduler)
 * to the v1 Scheduler interface. It uses the MesosSchedulerDriver under
 * the hood for interacting with Mesos. This class is thread-safe.
 */
public class V0Mesos implements Mesos {
  static {
    MesosNativeLibrary.load();
  }

  public V0Mesos(Scheduler scheduler, FrameworkInfo framework, String master) {
    this(scheduler, framework, master, null);
  }

  public V0Mesos(Scheduler scheduler,
                 FrameworkInfo framework,
                 String master,
                 Credential credential) {
    if (scheduler == null) {
      throw new NullPointerException("Not expecting a null scheduler");
    }

    if (framework == null) {
      throw new NullPointerException("Not expecting a null framework");
    }

    if (master == null) {
      throw new NullPointerException("Not expecting a null master");
    }

    this.scheduler = scheduler;
    this.framework = framework;
    this.master = master;
    this.credential = credential;

    initialize();
  }

  @Override
  public native void send(Call call);

  // This is currently a no-op for the driver as it does not expose semantics
  // to force reconnection.
  @Override
  public void reconnect() {}

  protected native void initialize();
  protected native void finalize();

  private final Scheduler scheduler;
  private final FrameworkInfo framework;
  private final String master;
  private final Credential credential;

  private long __mesos;
}
