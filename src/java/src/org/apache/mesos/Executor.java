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
 * Callback interface to be implemented by frameworks' executors.
 * Note that only one callback will be invoked at a time, so it is not
 * recommended that you block within a callback because it may cause a
 * deadlock.
 * <p>
 * Each callback includes a reference to the executor driver that was
 * used to run this executor. The reference will not change for the
 * duration of an executor (i.e., from the point you do
 * {@link ExecutorDriver#start} to the point that
 * {@link ExecutorDriver#join}  returns).
 * This is intended for convenience so that an executor
 * doesn't need to store a reference to the driver itself.
 */
public interface Executor {

  /**
   * Invoked once the executor driver has been able to successfully
   * connect with Mesos. In particular, a scheduler can pass some
   * data to its executors through the {@link ExecutorInfo#getData()}
   * field.
   *
   * @param driver        The executor driver that was registered and connected
   *                      to the Mesos cluster.
   * @param executorInfo  Describes information about the executor that was
   *                      registered.
   * @param frameworkInfo Describes the framework that was registered.
   * @param slaveInfo     Describes the slave that will be used to launch
   *                      the tasks for this executor.
   *
   * @see ExecutorDriver
   * @see MesosSchedulerDriver
   */
   // TODO(vinod): Add a new reregistered callback for when the executor
   //              re-connects with a restarted slave.
   void registered(ExecutorDriver driver,
                  ExecutorInfo executorInfo,
                  FrameworkInfo frameworkInfo,
                  SlaveInfo slaveInfo);

  /**
   * Invoked when the executor reregisters with a restarted slave.
   *
   * @param driver      The executor driver that was reregistered with the
   *                    Mesos master.
   * @param slaveInfo   Describes the slave that will be used to launch
   *                    the tasks for this executor.
   *
   * @see ExecutorDriver
   */
  void reregistered(ExecutorDriver driver, SlaveInfo slaveInfo);

  /**
   * Invoked when the executor becomes "disconnected" from the slave
   * (e.g., the slave is being restarted due to an upgrade).
   *
   * @param driver  The executor driver that was disconnected.
   */
  void disconnected(ExecutorDriver driver);

  /**
   * Invoked when a task has been launched on this executor (initiated
   * via {@link SchedulerDriver#launchTasks}. Note that this task can be
   * realized with a thread, a process, or some simple computation,
   * however, no other callbacks will be invoked on this executor
   * until this callback has returned.
   *
   * @param driver  The executor driver that launched the task.
   * @param task    Describes the task that was launched.
   *
   * @see ExecutorDriver
   * @see TaskInfo
   */
  void launchTask(ExecutorDriver driver, TaskInfo task);

  /**
   * Invoked when a task running within this executor has been killed
   * (via {@link org.apache.mesos.SchedulerDriver#killTask}). Note that no
   * status update will be sent on behalf of the executor, the executor is
   * responsible for creating a new TaskStatus (i.e., with TASK_KILLED)
   * and invoking {@link ExecutorDriver#sendStatusUpdate}.
   *
   * @param driver The executor driver that owned the task that was killed.
   * @param taskId The ID of the task that was killed.
   *
   * @see ExecutorDriver
   * @see TaskID
   */
  void killTask(ExecutorDriver driver, TaskID taskId);

  /**
   * Invoked when a framework message has arrived for this
   * executor. These messages are best effort; do not expect a
   * framework message to be retransmitted in any reliable fashion.
   *
   * @param driver  The executor driver that received the message.
   * @param data    The message payload.
   *
   * @see ExecutorDriver
   */
  void frameworkMessage(ExecutorDriver driver, byte[] data);

  /**
   * Invoked when the executor should terminate all of its currently
   * running tasks. Note that after Mesos has determined that an
   * executor has terminated any tasks that the executor did not send
   * terminal status updates for (e.g. TASK_KILLED, TASK_FINISHED,
   * TASK_FAILED, etc) a TASK_LOST status update will be created.
   *
   * @param driver  The executor driver that should terminate.
   *
   * @see ExecutorDriver
   */
  void shutdown(ExecutorDriver driver);

  /**
   * Invoked when a fatal error has occurred with the executor and/or
   * executor driver. The driver will be aborted BEFORE invoking this
   * callback.
   *
   * @param driver  The executor driver that was aborted due this error.
   * @param message The error message.
   *
   * @see ExecutorDriver
   */
  void error(ExecutorDriver driver, String message);
}
