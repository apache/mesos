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
import org.apache.mesos.scheduler.Protos.OfferConstraints;


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
   *
   * @return    The state of the driver after the call.
   *
   * @see Status
   */
  Status start();

  /**
   * Stops the scheduler driver. If the 'failover' flag is set to
   * false then it is expected that this framework will never
   * reconnect to Mesos. So Mesos will unregister the framework
   * and shutdown all its tasks and executors. If 'failover' is true,
   * all executors and tasks will remain running (for some framework
   * specific failover timeout) allowing the scheduler to reconnect
   * (possibly in the same process, or from a different process, for
   * example, on a different machine).
   *
   * @param failover    Whether framework failover is expected.
   *
   * @return            The state of the driver after the call.
   *
   * @see Status
   */
  Status stop(boolean failover);

  /**
   * Stops the scheduler driver assuming no failover. This will
   * cause Mesos to unregister the framework and shutdown all
   * its tasks and executors. Please see {@link #stop(boolean)}
   * for more details.
   *
   * @return The state of the driver after the call.
   */
  Status stop();

  /**
   * Aborts the driver so that no more callbacks can be made to the
   * scheduler. The semantics of abort and stop have deliberately been
   * separated so that code can detect an aborted driver (i.e., via
   * the return status of {@link #join}, see below), and instantiate
   * and start another driver if desired (from within the same
   * process).
   *
   * @return The state of the driver after the call.
   */
  Status abort();

  /**
   * Waits for the driver to be stopped or aborted, possibly
   * <i>blocking</i> the current thread indefinitely. The return status of
   * this function can be used to determine if the driver was aborted
   * (see mesos.proto for a description of Status).
   *
   * @return The state of the driver after the call.
   */
  Status join();

  /**
   * Starts and immediately joins (i.e., blocks on) the driver.
   *
   * @return The state of the driver after the call.
   */
  Status run();

  /**
   * Requests resources from Mesos (see mesos.proto for a description
   * of Request and how, for example, to request resources
   * from specific slaves). Any resources available are offered to the
   * framework via {@link Scheduler#resourceOffers} callback,
   * asynchronously.
   *
   * @param requests    The resource requests.
   *
   * @return            The state of the driver after the call.
   *
   * @see Request
   * @see Status
   */
  Status requestResources(Collection<Request> requests);

  /**
   * Launches the given set of tasks. Any remaining resources (i.e.,
   * those that are not used by the launched tasks or their executors)
   * will be considered declined. Note that this includes resources
   * used by tasks that the framework attempted to launch but failed
   * (with TASK_ERROR) due to a malformed task description. The
   * specified filters are applied on all unused resources (see
   * mesos.proto for a description of Filters). Available resources
   * are aggregated when multiple offers are provided. Note that all
   * offers must belong to the same slave. Invoking this function with
   * an empty collection of tasks declines offers in their entirety
   * (see {@link #declineOffer}).
   *
   * @param offerIds    The collection of offer IDs.
   * @param tasks       The collection of tasks to be launched.
   * @param filters     The filters to set for any remaining resources.
   *
   * @return            The state of the driver after the call.
   *
   * @see OfferID
   * @see TaskInfo
   * @see Filters
   * @see Status
   */
  Status launchTasks(Collection<OfferID> offerIds,
                     Collection<TaskInfo> tasks,
                     Filters filters);

  /**
   * Launches the given set of tasks. See above for details.
   * Note that this may add a default filter (see mesos.proto)
   * for the remaining resources. Notably the MesosSchedulerDriver
   * does so.
   *
   *
   * @param offerIds    The collection of offer IDs.
   * @param tasks       The collection of tasks to be launched.
   *
   * @return            The state of the driver after the call.
   */
  Status launchTasks(Collection<OfferID> offerIds, Collection<TaskInfo> tasks);

  /**
   * @deprecated Use {@link #launchTasks(Collection, Collection, Filters)} instead.
   *
   * @param offerId The offer ID.
   * @param tasks   The collection of tasks to be launched.
   * @param filters The filters to set for any remaining resources.
   *
   * @return        The state of the driver after the call.
   */
  Status launchTasks(OfferID offerId,
                     Collection<TaskInfo> tasks,
                     Filters filters);

  /**
   * @deprecated Use {@link #launchTasks(Collection, Collection)} instead.
   * Note that this may add a default filter (see mesos.proto)
   * for the remaining resources. Notably the MesosSchedulerDriver
   * does so.
   *
   * @param offerId The offer ID.
   * @param tasks   The collection of tasks to be launched.
   *
   * @return        The state of the driver after the call.
   */
  Status launchTasks(OfferID offerId, Collection<TaskInfo> tasks);

  /**
   * Kills the specified task. Note that attempting to kill a task is
   * currently not reliable. If, for example, a scheduler fails over
   * while it was attempting to kill a task it will need to retry in
   * the future Likewise, if unregistered / disconnected, the request
   * will be dropped (these semantics may be changed in the future).
   *
   * @param taskId  The ID of the task to be killed.
   *
   * @return        The state of the driver after the call.
   */
  Status killTask(TaskID taskId);

  /**
   * Accepts the given offers and performs a sequence of operations on
   * those accepted offers. See Offer.Operation in mesos.proto for the
   * set of available operations. Any remaining resources (i.e., those
   * that are not used by the launched tasks or their executors) will
   * be considered declined. Note that this includes resources used by
   * tasks that the framework attempted to launch but failed (with
   * TASK_ERROR) due to a malformed task description. The specified
   * filters are applied on all unused resources (see mesos.proto for
   * a description of Filters). Available resources are aggregated
   * when multiple offers are provided. Note that all offers must
   * belong to the same slave.
   *
   * @param offerIds    The collection of offer IDs.
   * @param operations  The collection of offer operations to perform.
   * @param filters     The filters to set for any remaining resources.
   *
   * @return            The state of the driver after the call.
   *
   * @see OfferID
   * @see Offer.Operation
   * @see Filters
   * @see Status
   */
  Status acceptOffers(Collection<OfferID> offerIds,
                      Collection<Offer.Operation> operations,
                      Filters filters);

  /**
   * Declines an offer in its entirety and applies the specified
   * filters on the resources (see mesos.proto for a description of
   * Filters). Note that this can be done at any time, it is not
   * necessary to do this within the {@link Scheduler#resourceOffers}
   * callback.
   *
   * @param offerId The ID of the offer to be declined.
   * @param filters The filters to set for any remaining resources.
   *
   * @return        The state of the driver after the call.
   *
   * @see OfferID
   * @see Filters
   * @see Status
   */
  Status declineOffer(OfferID offerId, Filters filters);

  /**
   * Declines an offer in its entirety. See above for details.
   *
   * @param offerId The ID of the offer to be declined.
   *
   * @return        The state of the driver after the call.
   *
   * @see OfferID
   * @see Status
   */
  Status declineOffer(OfferID offerId);

  /**
   * Removes all filters previously set by the framework (via launchTasks()
   * or declineOffer()) and clears the set of suppressed roles.
   *
   * NOTE: If the framework is not connected to the master, the set
   * of suppressed roles stored by the driver will be cleared, and an
   * up-to-date set of suppressed roles will be sent to the master
   * during re-registration.
   *
   * @return    The state of the driver after the call.
   *
   * @see Status
   */
  Status reviveOffers();

  /**
   * Removes filters for the specified roles and removes these roles from
   * the suppressed set. If the framework is not connected to the master,
   * an up-to-date set of suppressed roles will be sent to the master
   * during re-registration.
   *
   * @param roles The collection of the framework roles to be revivied.
   *              If empty, this method does nothing.
   *
   * @return    The state of the driver after the call.
   *
   * @see Status
   */
  Status reviveOffers(Collection<String> roles);

  /**
   * Informs Mesos master to stop sending offers to the framework (i.e.
   * to suppress all roles of the framework). To resume getting offers,
   * the scheduler can call reviveOffers() or set the suppressed roles
   * explicitly via updateFramework().
   *
   * NOTE: If the framework is not connected to the master, all the roles
   * will be added to the set of suppressed roles in the driver, and an
   * up-to-date suppressed roles set will be sent to the master during
   * re-registration.
   *
   * @return    The state of the driver after the call.
   *
   * @see Status
   */
  Status suppressOffers();

  /**
   * Adds the roles to the suppressed set. If the framework is not connected
   * to the master, an up-to-date set of suppressed roles will be sent to
   * the master during re-registration.
   *
   * @param roles The collection of framework roles to be suppressed.
   *              If empty, the method does nothing.
   *
   * @return    The state of the driver after the call.
   *
   * @see Status
   */
  Status suppressOffers(Collection<String> roles);

  /**
   * Acknowledges the status update. This should only be called
   * once the status update is processed durably by the scheduler.
   * Not that explicit acknowledgements must be requested via the
   * constructor argument, otherwise a call to this method will
   * cause the driver to crash.
   *
   * @param status  The status to acknowledge.
   *
   * @return        The state of the driver after the call.
   *
   * @see TaskStatus
   */
  Status acknowledgeStatusUpdate(TaskStatus status);

  /**
   * Sends a message from the framework to one of its executors. These
   * messages are best effort; do not expect a framework message to be
   * retransmitted in any reliable fashion.
   *
   * @param executorId  The ID of the executor to send the message to.
   * @param slaveId     The ID of the slave that is running the executor.
   * @param data        The message.
   *
   * @return            The state of the driver after the call.
   *
   * @see ExecutorID
   * @see SlaveID
   */
  Status sendFrameworkMessage(ExecutorID executorId,
                              SlaveID slaveId,
                              byte[] data);

  /**
   * Allows the framework to query the status for non-terminal tasks.
   * This causes the master to send back the latest task status for
   * each task in 'statuses', if possible. Tasks that are no longer
   * known will result in a TASK_LOST update. If statuses is empty,
   * then the master will send the latest status for each task
   * currently known.
   *
   * @param statuses    The collection of non-terminal TaskStatuses to reconcile.
   *
   * @return            The state of the driver after the call.
   *
   * @see TaskStatus
   * @see SlaveID
   */
  Status reconcileTasks(Collection<TaskStatus> statuses);

  /*
   * Requests Mesos master to change the `FrameworkInfo`, the set of suppressed
   * roles and the offer constraints. The driver will store the new
   * `FrameworkInfo`, the new set of suppressed roles and the new offer
   * constraints, and all subsequent re-registrations will use them.
   *
   * NOTE: If the supplied info is invalid or fails authorization,
   * or the supplied offer constraints are not valid, the `error()` callback
   * will be invoked asynchronously (after the master replies with a
   * `FrameworkErrorMessage`). Note that validity of non-empty (i.e.
   * not default-constructed) offer constraints may depend on master flags.
   *
   * NOTE: This must be called after initial registration with the
   * master completes and the `FrameworkID` is assigned. The assigned
   * `FrameworkID` must be set in `frameworkInfo`.
   *
   * NOTE: The `FrameworkInfo.user` and `FrameworkInfo.hostname`
   * fields will be auto-populated using the same approach used
   * during driver initialization.
   *
   * @param frameworkInfo     The new FrameworkInfo.
   * @param suppressedRoles   The new list of suppressed roles.
   * @param offerConstraints  The new offer constraints.
   *
   * @return                  The state of the driver after the call.
   *
   * @see FrameworkInfo
   * @see OfferConstraints
   */
  Status updateFramework(FrameworkInfo frameworkInfo,
                         Collection<String> suppressedRoles,
                         OfferConstraints offerConstraints);

 /**
  * @deprecated
  * To call UPDATE_FRAMEWORK without setting offer constraints,
  * use the new `updateFramework()` signature and pass empty constraints
  * (for example, the ones returned by `OfferConstraints.getDefaultInstance()`).
  */
  @Deprecated
  Status updateFramework(FrameworkInfo frameworkInfo,
                         Collection<String> suppressedRoles);
}
