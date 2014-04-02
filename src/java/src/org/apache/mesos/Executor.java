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
 *
 * Each callback includes a reference to the executor driver that was
 * used to run this executor. The reference will not change for the
 * duration of an executor (i.e., from the point you do {@link
 * ExecutorDriver#start} to the point that {@link ExecutorDriver#join}
 * returns). This is intended for convenience so that an executor
 * doesn't need to store a reference to the driver itself.
 */
public interface Executor {
  /**
   * Invoked once the executor driver has been able to successfully
   * connect with Mesos. In particular, a scheduler can pass some
   * data to it's executors through the {@link ExecutorInfo#data}
   * field. TODO(vinod): Add a new reregistered callback for when the executor
   * re-connects with a restarted slave.
   *
   * @param driver The driver that was used to start this executor.
   * @param executorInfo The executor info that was used to start this executor.
   * @param frameworkInfo The framework that started this executor.
   * @param slaveInfo The slave this executor is registered with.
   */
  void registered(ExecutorDriver driver,
                  ExecutorInfo executorInfo,
                  FrameworkInfo frameworkInfo,
                  SlaveInfo slaveInfo);

  /**
   * Invoked when the executor re-registers with a restarted slave.
   *
   * @param driver The driver that was used to start this executor.
   * @param slaveInfo The slave this executor re-registered with.
   */
  void reregistered(ExecutorDriver driver, SlaveInfo slaveInfo);

  /**
   * Invoked when the executor becomes "disconnected" from the slave
   * (e.g., the slave is being restarted due to an upgrade).
   *
   * @param driver The driver that was used to start this executor.
   */
  void disconnected(ExecutorDriver driver);

  /**
   * Invoked when a task has been launched on this executor (initiated
   * via {@link Scheduler#launchTasks}. Note that this task can be
   * realized with a thread, a process, or some simple computation,
   * however, no other callbacks will be invoked on this executor
   * until this callback has returned.
   *
   * @param driver The driver that was used to start this executor.
   * @param task The info about the task that was launched.
   */
  void launchTask(ExecutorDriver driver, TaskInfo task);

  /**
   * Invoked when a task running within this executor has been killed
   * (via {@link SchedulerDriver#killTask}). Note that no status
   * update will be sent on behalf of the executor, the executor is
   * responsible for creating a new TaskStatus (i.e., with
   * TASK_KILLED) and invoking {@link
   * ExecutorDriver#sendStatusUpdate}.
   *
   * @param driver The driver that was used to start this executor.
   * @param taskId The ID of the task that was killed.
   */
  void killTask(ExecutorDriver driver, TaskID taskId);

  /**
   * Invoked when a framework message has arrived for this
   * executor. These messages are best effort; do not expect a
   * framework message to be retransmitted in any reliable fashion.
   *
   * @param driver The driver that was used to start this executor.
   * @param data The message data.
   */
  void frameworkMessage(ExecutorDriver driver, byte[] data);

  /**
   * Invoked when the executor should terminate all of it's currently
   * running tasks. Note that after a Mesos has determined that an
   * executor has terminated any tasks that the executor did not send
   * terminal status updates for (e.g., TASK_KILLED, TASK_FINISHED,
   * TASK_FAILED, etc) a TASK_LOST status update will be created.
   *
   * @param driver The driver that was used to start this executor.
   */
  void shutdown(ExecutorDriver driver);

  /**
   * Invoked when a fatal error has occured with the executor and/or
   * executor driver. The driver will be aborted BEFORE invoking this
   * callback.
   *
   * @param driver The driver that was used to start this executor.
   * @param message The error message.
   */
  void error(ExecutorDriver driver, String message);
}
