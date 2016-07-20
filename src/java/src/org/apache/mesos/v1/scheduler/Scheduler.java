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

import org.apache.mesos.v1.scheduler.Protos.Event;

/**
 * Callback interface to be implemented by schedulers.
 * Note that only one callback will be invoked at a time,
 * so it is not recommended that you block within a callback because
 * it may cause a deadlock.
 * <p>
 * Each callback includes a reference to the Mesos interface that was
 * used to run this scheduler. The reference will not change for the
 * duration of a scheduler from the time it is instantiated.
 * This is intended for convenience so that a scheduler doesn't need to
 * store a reference to the interface itself.
 */

public interface Scheduler {
  /**
   * Invoked when a connection is established with the master upon a
   * master (re-)detection.
   */
  void connected(Mesos mesos);

  /**
   * Invoked when no master is detected or when the existing persistent
   * connection is interrupted.
   */
  void disconnected(Mesos mesos);

  /**
   * Invoked when a new event is received from the Mesos master.
   */
  void received(Mesos mesos, Event event);
}
