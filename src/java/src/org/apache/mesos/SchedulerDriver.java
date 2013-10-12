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
import java.util.Map;


/**
 * Abstract interface for connecting a scheduler to Mesos. This
 * interface is used both to manage the scheduler's lifecycle (start
 * it, stop it, or wait for it to finish) and to interact with Mesos
 * (e.g., launch tasks, kill tasks, etc.).
 */
public interface SchedulerDriver {
  /**
   * Starts the scheduler driver. This needs to be called before any
   * other driver calls are made.
   */
  Status start();

  /**
   * Stops the scheduler driver. If the 'failover' flag is set to
   * false then it is expected that this framework will never
   * reconnect to Mesos and all of it's executors and tasks can be
   * terminated. Otherwise, all executors and tasks will remain
   * running (for some master specified failover timeout) allowing the
   * scheduler to reconnect (possibly in the same process, or from a
   * different process, for example, on a different machine).
   */
  Status stop(boolean failover);

  /**
   * Stops the scheduler driver assuming no failover.
   */
  Status stop();

  /**
   * Aborts the driver so that no more callbacks can be made to the
   * scheduler. The semantics of abort and stop have deliberately been
   * separated so that code can detect an aborted driver (i.e., via
   * the return status of {@link #join}, see below), and instantiate
   * and start another driver if desired (from within the same
   * process).
   */
  Status abort();

  /**
   * Waits for the driver to be stopped or aborted, possibly
   * _blocking_ the current thread indefinitely. The return status of
   * this function can be used to determine if the driver was aborted
   * (see mesos.proto for a description of Status).
   */
  Status join();

  /**
   * Starts and immediately joins (i.e., blocks on) the driver.
   */
  Status run();

  /**
   * Requests resources from Mesos (see mesos.proto for a description
   * of Request and how, for example, to request resources
   * from specific slaves). Any resources available are offered to the
   * framework via {@link Scheduler#resourceOffers} callback,
   * asynchronously.
   */
  Status requestResources(Collection<Request> requests);

  /**
   * Launches the given set of tasks. Any resources remaining (i.e.,
   * not used by the tasks or their executors) will be considered
   * declined. The specified filters are applied on all unused
   * resources (see mesos.proto for a description of Filters).
   * Invoking this function with an empty collection of tasks declines
   * this offer in its entirety (see {@link #declineOffer}. Note that
   * currently tasks can only be launched per offer. In the future,
   * frameworks will be allowed to aggregate offers (resources) to
   * launch their tasks.
   */
  Status launchTasks(OfferID offerId,
                     Collection<TaskInfo> tasks,
                     Filters filters);

  /**
   * Launches the given set of tasks. See above for details.
   */
  Status launchTasks(OfferID offerId, Collection<TaskInfo> tasks);

  /**
   * Kills the specified task. Note that attempting to kill a task is
   * currently not reliable. If, for example, a scheduler fails over
   * while it was attempting to kill a task it will need to retry in
   * the future (these semantics may be changed in the future).
   */
  Status killTask(TaskID taskId);

  /**
   * Declines an offer in its entirety and applies the specified
   * filters on the resources (see mesos.proto for a description of
   * Filters). Note that this can be done at any time, it is not
   * necessary to do this within the {@link Scheduler#resourceOffers}
   * callback.
   */
  Status declineOffer(OfferID offerId, Filters filters);

  /**
   * Declines an offer in its entirety. See above for details.
   */
  Status declineOffer(OfferID offerId);

  /**
   * Removes all filters, previously set by the framework (via {@link
   * #launchTasks}). This enables the framework to receive offers
   * from those filtered slaves.
   */
  Status reviveOffers();

  /**
   * Sends a message from the framework to one of its executors. These
   * messages are best effort; do not expect a framework message to be
   * retransmitted in any reliable fashion.
   */
  Status sendFrameworkMessage(ExecutorID executorId,
                              SlaveID slaveId,
                              byte[] data);

  /**
   * Reconciliation of tasks causes the master to send status updates for tasks
   * whose status differs from the status sent here.
   */
  Status reconcileTasks(Collection<TaskStatus> statuses);
}
