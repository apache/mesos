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

import org.apache.mesos.v1.scheduler.Protos.Call;

/**
 * Abstract interface for connecting a scheduler to Mesos. This interface
 * is used to send Call's to Mesos (e.g., launch tasks, kill tasks, etc.).
 */
public interface Mesos {
  /**
   * Sends a Call to the Mesos master. The scheduler should only invoke this
   * method once it has received the 'connected' callback. Otherwise, all calls
   * would be dropped while disconnected.
   *
   * Some local validation of calls is performed which may generate
   * events without ever being sent to the master.
   *
   * These comments are copied from include/mesos/v1/scheduler.hpp and should
   * be kept in sync.
   */
  void send(Call call);

  /**
   * Force a reconnection with the Mesos master.
   *
   * In the case of a one-way network partition, the connection between the
   * scheduler and master might not necessarily break. If the scheduler detects
   * a partition, due to lack of `HEARTBEAT` events (e.g., 5) within a time
   * window, it can explicitly ask the library to force a reconnection with
   * the master.
   *
   * This call would be ignored if the scheduler is already disconnected with
   * the master (e.g., no new master has been elected). Otherwise, the scheduler
   * would get a 'disconnected' callback followed by a 'connected' callback.
   *
   * These comments are copied from include/mesos/v1/scheduler.hpp and should
   * be kept in sync.
   */
  void reconnect();
}
