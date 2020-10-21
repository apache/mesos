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

import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.Map;


/**
 * Concrete implementation of a SchedulerDriver that connects a
 * Scheduler with a Mesos master. The MesosSchedulerDriver is
 * thread-safe.
 * <p>
 * Note that scheduler failover is supported in Mesos. After a
 * scheduler is registered with Mesos it may failover (to a new
 * process on the same machine or across multiple machines) by
 * creating a new driver with the ID given to it in {@link
 * Scheduler#registered}.
 * <p>
 * The driver is responsible for invoking the Scheduler callbacks as
 * it communicates with the Mesos master.
 * <p>
 * Note that blocking on the MesosSchedulerDriver (e.g., via {@link
 * #join}) doesn't affect the scheduler callbacks in anyway because
 * they are handled by a different thread.
 * <p>
 * <p>
 * Note that the driver uses GLOG to do its own logging. GLOG flags can
 * be set via environment variables, prefixing the flag name with
 * "GLOG_", e.g., "GLOG_v=1". For Mesos specific logging flags see
 * src/logging/flags.hpp. Mesos flags can also be set via environment
 * variables, prefixing the flag name with "MESOS_", e.g.,
 * "MESOS_QUIET=1".
 * <p>
 * See src/examples/java/TestFramework.java for an example of using
 * the MesosSchedulerDriver.
 */
public class MesosSchedulerDriver implements SchedulerDriver {
  static {
    MesosNativeLibrary.load();
  }

  /**
   * Creates a new driver for the specified scheduler. The master
   * should be one of:
   * <pre>
   * {@code
   *     host:port
   *     zk://host1:port1,host2:port2,.../path
   *     zk://username:password@host1:port1,host2:port2,.../path
   *     file:///path/to/file (where file contains one of the above)
   * }
   * </pre>
   * <p>
   * The driver will attempt to "failover" if the specified
   * FrameworkInfo includes a valid FrameworkID.
   * <p>
   * Any Mesos configuration options are read from environment
   * variables, as well as any configuration files found through the
   * environment variables.
   * <p>
   *
   * @param scheduler The scheduler implementation which callbacks are invoked
   *                  upon scheduler events.
   * @param framework The frameworkInfo describing the current framework.
   * @param master    The address to the currently active Mesos master.
   */
   // TODO(vinod): Deprecate this in favor the constructor that takes
   //              'credential' as parameter.
  public MesosSchedulerDriver(Scheduler scheduler,
                              FrameworkInfo framework,
                              String master) {
    if (scheduler == null) {
      throw new NullPointerException("Not expecting a null Scheduler");
    }

    if (framework == null) {
      throw new NullPointerException("Not expecting a null FrameworkInfo");
    }

    if (master == null) {
      throw new NullPointerException("Not expecting a null master");
    }

    this.scheduler = scheduler;
    this.framework = framework;
    this.suppressedRoles = null;
    this.master = master;
    this.implicitAcknowledgements = true;
    this.credential = null;

    initialize();
  }

  /**
   * Same as the other constructors, except that it accepts the newly
   * introduced 'credential' parameter.
   *
   * @param scheduler   The scheduler implementation which callbacks are invoked
   *                    upon scheduler events.
   * @param framework   The frameworkInfo describing the current framework.
   * @param master      The address to the currently active Mesos master.
   * @param credential  The credentials that will be used used to authenticate
   *                    calls from this scheduler.
   */
  public MesosSchedulerDriver(Scheduler scheduler,
                              FrameworkInfo framework,
                              String master,
                              Credential credential) {

    if (scheduler == null) {
      throw new NullPointerException("Not expecting a null Scheduler");
    }

    if (framework == null) {
      throw new NullPointerException("Not expecting a null FrameworkInfo");
    }

    if (master == null) {
      throw new NullPointerException("Not expecting a null master");
    }

    if (credential == null) {
      throw new NullPointerException("Not expecting a null credential");
    }

    this.scheduler = scheduler;
    this.framework = framework;
    this.suppressedRoles = null;
    this.master = master;
    this.implicitAcknowledgements = true;
    this.credential = credential;

    initialize();
  }

  /**
   * Same as the other constructors, except that it accepts the newly
   * introduced 'implicitAcknowledgements' parameter.
   *
   * @param scheduler   The scheduler implementation which callbacks are invoked
   *                    upon scheduler events.
   * @param framework   The frameworkInfo describing the current framework.
   * @param master      The address to the currently active Mesos master.
   * @param implicitAcknowledgements  Whether the driver should send
   *            acknowledgements on behalf of the scheduler. Setting this to
   *            false allows schedulers to perform their own acknowledgements,
   *            which enables asynchronous / batch processing of status updates.
   */
  public MesosSchedulerDriver(Scheduler scheduler,
                              FrameworkInfo framework,
                              String master,
                              boolean implicitAcknowledgements) {

    if (scheduler == null) {
      throw new NullPointerException("Not expecting a null Scheduler");
    }

    if (framework == null) {
      throw new NullPointerException("Not expecting a null FrameworkInfo");
    }

    if (master == null) {
      throw new NullPointerException("Not expecting a null master");
    }

    this.scheduler = scheduler;
    this.framework = framework;
    this.suppressedRoles = null;
    this.master = master;
    this.implicitAcknowledgements = implicitAcknowledgements;
    this.credential = null;

    initialize();
  }

  /**
   * Same as the other constructors, except that it accepts the newly
   * introduced 'implicitAcknowledgements' and 'credentials' parameters.
   *
   * @param scheduler   The scheduler implementation which callbacks are invoked
   *                    upon scheduler events.
   * @param framework   The frameworkInfo describing the current framework.
   * @param master      The address to the currently active Mesos master.
   * @param implicitAcknowledgements  Whether the driver should send
   *            acknowledgements on behalf of the scheduler. Setting this to
   *            false allows schedulers to perform their own acknowledgements,
   *            which enables asynchronous / batch processing of status updates.
   * @param credential  The credentials that will be used used to authenticate
   *                    calls from this scheduler.
   */
  public MesosSchedulerDriver(Scheduler scheduler,
                              FrameworkInfo framework,
                              String master,
                              boolean implicitAcknowledgements,
                              Credential credential) {

    if (scheduler == null) {
      throw new NullPointerException("Not expecting a null Scheduler");
    }

    if (framework == null) {
      throw new NullPointerException("Not expecting a null FrameworkInfo");
    }

    if (master == null) {
      throw new NullPointerException("Not expecting a null master");
    }

    if (credential == null) {
      throw new NullPointerException("Not expecting a null credential");
    }

    this.scheduler = scheduler;
    this.framework = framework;
    this.suppressedRoles = null;
    this.master = master;
    this.implicitAcknowledgements = implicitAcknowledgements;
    this.credential = credential;

    initialize();
  }

  /**
   * Same as the other constructors, except that it accepts the newly
   * introduced 'suppressedRoles' parameter.
   *
   * @param scheduler   The scheduler implementation which callbacks are invoked
   *                    upon scheduler events.
   * @param framework   The frameworkInfo describing the current framework.
   * @param suppressedRoles  The collection of initially suppressed roles.
   * @param master      The address to the currently active Mesos master.
   * @param implicitAcknowledgements  Whether the driver should send
   *            acknowledgements on behalf of the scheduler. Setting this to
   *            false allows schedulers to perform their own acknowledgements,
   *            which enables asynchronous / batch processing of status updates.
   */
  public MesosSchedulerDriver(Scheduler scheduler,
                              FrameworkInfo framework,
                              Collection<String> suppressedRoles,
                              String master,
                              boolean implicitAcknowledgements) {

    if (scheduler == null) {
      throw new NullPointerException("Not expecting a null Scheduler");
    }

    if (framework == null) {
      throw new NullPointerException("Not expecting a null FrameworkInfo");
    }

    if (suppressedRoles == null) {
      throw new NullPointerException("Not expecting a null suppressedRoles");
    }

    if (master == null) {
      throw new NullPointerException("Not expecting a null master");
    }

    this.scheduler = scheduler;
    this.framework = framework;
    this.suppressedRoles = suppressedRoles;
    this.master = master;
    this.implicitAcknowledgements = implicitAcknowledgements;
    this.credential = null;

    initialize();
  }


  /**
   * Same as the other constructors, except that it accepts the newly
   * introduced 'suppressedRoles' parameter.
   *
   * @param scheduler   The scheduler implementation which callbacks are invoked
   *                    upon scheduler events.
   * @param framework   The frameworkInfo describing the current framework.
   * @param suppressedRoles  The collection of initially suppressed roles.
   * @param master      The address to the currently active Mesos master.
   * @param implicitAcknowledgements  Whether the driver should send
   *            acknowledgements on behalf of the scheduler. Setting this to
   *            false allows schedulers to perform their own acknowledgements,
   *            which enables asynchronous / batch processing of status updates.
   * @param credential  The credentials that will be used used to authenticate
   *                    calls from this scheduler.
   */
  public MesosSchedulerDriver(Scheduler scheduler,
                              FrameworkInfo framework,
                              Collection<String> suppressedRoles,
                              String master,
                              boolean implicitAcknowledgements,
                              Credential credential) {

    if (scheduler == null) {
      throw new NullPointerException("Not expecting a null Scheduler");
    }

    if (framework == null) {
      throw new NullPointerException("Not expecting a null FrameworkInfo");
    }

    if (suppressedRoles == null) {
      throw new NullPointerException("Not expecting a null suppressedRoles");
    }

    if (master == null) {
      throw new NullPointerException("Not expecting a null master");
    }

    if (credential == null) {
      throw new NullPointerException("Not expecting a null credential");
    }

    this.scheduler = scheduler;
    this.framework = framework;
    this.suppressedRoles = suppressedRoles;
    this.master = master;
    this.implicitAcknowledgements = implicitAcknowledgements;
    this.credential = credential;

    initialize();
  }


  public native Status start();

  public native Status stop(boolean failover);

  public Status stop() {
    return stop(false);
  }

  public native Status abort();

  public native Status join();

  public Status run() {
    Status status = start();
    return status != Status.DRIVER_RUNNING ? status : join();
  }

  public native Status requestResources(Collection<Request> requests);

  public Status launchTasks(OfferID offerId,
                            Collection<TaskInfo> tasks) {
    return launchTasks(offerId, tasks, Filters.newBuilder().build());
  }

  public native Status launchTasks(OfferID offerId,
                                   Collection<TaskInfo> tasks,
                                   Filters filters);
  public Status launchTasks(Collection<OfferID> offerIds,
                            Collection<TaskInfo> tasks) {
    return launchTasks(offerIds, tasks, Filters.newBuilder().build());
  }

  public native Status launchTasks(Collection<OfferID> offerIds,
                                   Collection<TaskInfo> tasks,
                                   Filters filters);

  public native Status killTask(TaskID taskId);

  public native Status acceptOffers(Collection<OfferID> offerIds,
                                    Collection<Offer.Operation> operations,
                                    Filters filters);

  public Status declineOffer(OfferID offerId) {
    return declineOffer(offerId, Filters.newBuilder().build());
  }

  public native Status declineOffer(OfferID offerId, Filters filters);

  public native Status reviveOffers();

  public native Status reviveOffers(Collection<String> roles);

  public native Status suppressOffers();

  public native Status suppressOffers(Collection<String> roles);

  public native Status acknowledgeStatusUpdate(TaskStatus status);

  public native Status sendFrameworkMessage(ExecutorID executorId,
                                            SlaveID slaveId,
                                            byte[] data);

  public native Status reconcileTasks(Collection<TaskStatus> statuses);

  /**
   * @deprecated Replaced by
   * {@link #updateFramework(FrameworkInfo, Collection, OfferConstraints)}
   *
   * NOTE: The underlying JNI method exists only to maintain compatibility
   * of newer versions of libmesos-java.so with older versions of mesos.jar
   */
  @Deprecated
  public native Status updateFramework(FrameworkInfo frameworkInfo,
                                       Collection<String> suppressedRoles);

  public Status updateFramework(FrameworkInfo frameworkInfo,
                                Collection<String> suppressedRoles,
                                OfferConstraints offerConstraints) {
    return updateFrameworkWithConstraints(
        frameworkInfo, suppressedRoles, offerConstraints);
  }

  /*
   * NOTE: This method exists only because an `updateFramework()` signature
   * with added offer constraints needs to have a different name, due to the
   * `extern "C"` linkage of JNI method implementations.
   */
  private native Status updateFrameworkWithConstraints(
      FrameworkInfo frameworkInfo,
      Collection<String> suppressedRoles,
      OfferConstraints offerConstraints);

  protected native void initialize();
  protected native void finalize();

  private final Scheduler scheduler;
  private final FrameworkInfo framework;
  private final Collection<String> suppressedRoles;
  private final String master;
  private final boolean implicitAcknowledgements;
  private final Credential credential;

  private long __scheduler;
  private long __driver;
}
