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


/**
 * Abstract interface for connecting an executor to Mesos. This
 * interface is used both to manage the executor's lifecycle (start
 * it, stop it, or wait for it to finish) and to interact with Mesos
 * (e.g., send status updates, send framework messages, etc.).
 */
public interface ExecutorDriver {
  /**
   * Starts the executor driver. This needs to be called before any
   * other driver calls are made.
   *
   * @return    The state of the driver after the call.
   *
   * @see Status
   */
  public Status start();

  /**
   * Stops the executor driver.
   *
   * @return    The state of the driver after the call.
   *
   * @see Status
   */
  public Status stop();

  /**
   * Aborts the driver so that no more callbacks can be made to the
   * executor. The semantics of abort and stop have deliberately been
   * separated so that code can detect an aborted driver (i.e., via
   * the return status of {@link ExecutorDriver#join}, see below),
   * and instantiate and start another driver if desired (from within
   * the same process ... although this functionality is currently not
   * supported for executors).
   *
   * @return    The state of the driver after the call.
   *
   * @see Status
   */
  public Status abort();

  /**
   * Waits for the driver to be stopped or aborted, possibly
   * _blocking_ the current thread indefinitely. The return status of
   * this function can be used to determine if the driver was aborted
   * (see mesos.proto for a description of Status).
   *
   * @return    The state of the driver after the call.
   *
   * @see Status
   */
  public Status join();

  /**
   * Starts and immediately joins (i.e., blocks on) the driver.
   *
   * @return    The state of the driver after the call.
   *
   * @see Status
   */
  public Status run();

  /**
   * Sends a status update to the framework scheduler, retrying as
   * necessary until an acknowledgement has been received or the
   * executor is terminated (in which case, a TASK_LOST status update
   * will be sent). See {@link Scheduler#statusUpdate} for more
   * information about status update acknowledgements.
   *
   * @param status  The status update to send.
   *
   * @return        The state of the driver after the call.
   *
   * @see Status
   */
  public Status sendStatusUpdate(TaskStatus status);

  /**
   * Sends a message to the framework scheduler. These messages are
   * best effort; do not expect a framework message to be
   * retransmitted in any reliable fashion.
   *
   * @param data    The message payload.
   *
   * @return        The state of the driver after the call.
   *
   * @see Status
   */
  public Status sendFrameworkMessage(byte[] data);
}
