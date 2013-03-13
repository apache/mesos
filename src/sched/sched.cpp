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

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <iostream>
#include <map>
#include <string>
#include <sstream>

#include <mesos/scheduler.hpp>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/fatal.hpp>
#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/uuid.hpp>

#include "configurator/configuration.hpp"
#include "configurator/configurator.hpp"

#include "common/lock.hpp"
#include "common/type_utils.hpp"

#include "detector/detector.hpp"

#include "flags/flags.hpp"

#include "local/local.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "messages/messages.hpp"

using namespace mesos;
using namespace mesos::internal;

using namespace process;

using std::map;
using std::string;
using std::vector;

using process::wait; // Necessary on some OS's to disambiguate.


namespace mesos {
namespace internal {

// The scheduler process (below) is responsible for interacting with
// the master and responding to Mesos API calls from scheduler
// drivers. In order to allow a message to be sent back to the master
// we allow friend functions to invoke 'send', 'post', etc. Therefore,
// we must make sure that any necessary synchronization is performed.

class SchedulerProcess : public ProtobufProcess<SchedulerProcess>
{
public:
  SchedulerProcess(MesosSchedulerDriver* _driver,
                   Scheduler* _scheduler,
                   const FrameworkInfo& _framework,
                   const string& _url,
                   pthread_mutex_t* _mutex,
                   pthread_cond_t* _cond)
    : ProcessBase(ID::generate("scheduler")),
      driver(_driver),
      scheduler(_scheduler),
      framework(_framework),
      url(_url),
      mutex(_mutex),
      cond(_cond),
      failover(_framework.has_id() && !framework.id().value().empty()),
      master(UPID()),
      connected(false),
      aborted(false),
      // TODO(benh): Add Try().
      detector(Error("uninitialized"))
  {}

  virtual ~SchedulerProcess() {}

protected:
  virtual void initialize()
  {
    // The master detector needs to be created after this process is
    // running so that the "master detected" message is not dropped.
    // TODO(benh): Get access to flags so that we can decide whether
    // or not to make ZooKeeper verbose.
    detector = MasterDetector::create(url, self(), false);
    if (detector.isError()) {
      driver->abort();
      scheduler->error(driver, detector.error());
      return;
    }

    install<NewMasterDetectedMessage>(
        &SchedulerProcess::newMasterDetected,
        &NewMasterDetectedMessage::pid);

    install<NoMasterDetectedMessage>(
        &SchedulerProcess::noMasterDetected);

    install<FrameworkRegisteredMessage>(
        &SchedulerProcess::registered,
        &FrameworkRegisteredMessage::framework_id,
        &FrameworkRegisteredMessage::master_info);

    install<FrameworkReregisteredMessage>(
        &SchedulerProcess::reregistered,
        &FrameworkReregisteredMessage::framework_id,
        &FrameworkReregisteredMessage::master_info);

    install<ResourceOffersMessage>(
        &SchedulerProcess::resourceOffers,
        &ResourceOffersMessage::offers,
        &ResourceOffersMessage::pids);

    install<RescindResourceOfferMessage>(
        &SchedulerProcess::rescindOffer,
        &RescindResourceOfferMessage::offer_id);

    install<StatusUpdateMessage>(
        &SchedulerProcess::statusUpdate,
        &StatusUpdateMessage::update,
        &StatusUpdateMessage::pid);

    install<LostSlaveMessage>(
        &SchedulerProcess::lostSlave,
        &LostSlaveMessage::slave_id);

    install<ExecutorToFrameworkMessage>(
        &SchedulerProcess::frameworkMessage,
        &ExecutorToFrameworkMessage::slave_id,
        &ExecutorToFrameworkMessage::framework_id,
        &ExecutorToFrameworkMessage::executor_id,
        &ExecutorToFrameworkMessage::data);

    install<FrameworkErrorMessage>(
        &SchedulerProcess::error,
        &FrameworkErrorMessage::message);
  }

  virtual void finalize()
  {
    if (detector.isSome()) {
      MasterDetector::destroy(detector.get());
    }
  }

  void newMasterDetected(const UPID& pid)
  {
    VLOG(1) << "New master at " << pid;

    master = pid;
    link(master);

    connected = false;
    doReliableRegistration();
  }

  void noMasterDetected()
  {
    VLOG(1) << "No master detected, waiting for another master";

    // In this case, we don't actually invoke Scheduler::error
    // since we might get reconnected to a master imminently.
    connected = false;
    master = UPID();

    scheduler->disconnected(driver);
  }

  void registered(const FrameworkID& frameworkId, const MasterInfo& masterInfo)
  {
    if (aborted) {
      VLOG(1) << "Ignoring framework registered message because "
              << "the driver is aborted!";
      return;
    }

    if (connected) {
      VLOG(1) << "Ignoring framework registered message because "
              << "the driver is already connected!";
      return;
    }

    VLOG(1) << "Framework registered with " << frameworkId;

    framework.mutable_id()->MergeFrom(frameworkId);

    connected = true;
    failover = false;

    scheduler->registered(driver, frameworkId, masterInfo);
  }

  void reregistered(const FrameworkID& frameworkId, const MasterInfo& masterInfo)
  {
    if (aborted) {
      VLOG(1) << "Ignoring framework re-registered message because "
              << "the driver is aborted!";
      return;
    }

    if (connected) {
      VLOG(1) << "Ignoring framework re-registered message because "
              << "the driver is already connected!";
      return;
    }

    VLOG(1) << "Framework re-registered with " << frameworkId;

    CHECK(framework.id() == frameworkId);

    connected = true;
    failover = false;

    scheduler->reregistered(driver, masterInfo);
  }

  void doReliableRegistration()
  {
    if (connected || !master) {
      return;
    }

    if (!framework.has_id() || framework.id() == "") {
      // Touched for the very first time.
      RegisterFrameworkMessage message;
      message.mutable_framework()->MergeFrom(framework);
      send(master, message);
    } else {
      // Not the first time, or failing over.
      ReregisterFrameworkMessage message;
      message.mutable_framework()->MergeFrom(framework);
      message.set_failover(failover);
      send(master, message);
    }

    delay(Seconds(1.0), self(), &Self::doReliableRegistration);
  }

  void resourceOffers(const vector<Offer>& offers,
                      const vector<string>& pids)
  {
    if (aborted) {
      VLOG(1) << "Ignoring resource offers message because "
              << "the driver is aborted!";
      return;
    }

    VLOG(1) << "Received " << offers.size() << " offers";

    CHECK(offers.size() == pids.size());

    // Save the pid associated with each slave (one per offer) so
    // later we can send framework messages directly.
    for (size_t i = 0; i < offers.size(); i++) {
      UPID pid(pids[i]);
      // Check if parse failed (e.g., due to DNS).
      if (pid != UPID()) {
        VLOG(2) << "Saving PID '" << pids[i] << "'";
        savedOffers[offers[i].id()][offers[i].slave_id()] = pid;
      } else {
        VLOG(2) << "Failed to parse PID '" << pids[i] << "'";
      }
    }

    scheduler->resourceOffers(driver, offers);
  }

  void rescindOffer(const OfferID& offerId)
  {
    if (aborted) {
      VLOG(1) << "Ignoring rescind offer message because "
              << "the driver is aborted!";
      return;
    }

    VLOG(1) << "Rescinded offer " << offerId;

    savedOffers.erase(offerId);

    scheduler->offerRescinded(driver, offerId);
  }

  void statusUpdate(const StatusUpdate& update, const UPID& pid)
  {
    const TaskStatus& status = update.status();

    if (aborted) {
      VLOG(1) << "Ignoring task status update message because "
              << "the driver is aborted!";
      return;
    }

    VLOG(1) << "Status update: task " << status.task_id()
            << " of framework " << update.framework_id()
            << " is now in state " << status.state();

    CHECK(framework.id() == update.framework_id());

    // TODO(benh): Note that this maybe a duplicate status update!
    // Once we get support to try and have a more consistent view
    // of what's running in the cluster, we'll just let this one
    // slide. The alternative is possibly dealing with a scheduler
    // failover and not correctly giving the scheduler it's status
    // update, which seems worse than giving a status update
    // multiple times (of course, if a scheduler re-uses a TaskID,
    // that could be bad.

    scheduler->statusUpdate(driver, status);

    // Acknowledge the status update.
    // NOTE: We do a dispatch here instead of directly sending the ACK because,
    // we want to avoid sending the ACK if the driver was aborted when we
    // made the statusUpdate call. This works because, the 'abort' message will
    // be enqueued before the ACK message is processed.
    if (pid > 0) {
      dispatch(self(), &Self::statusUpdateAcknowledgement, update, pid);
    }
  }

  void statusUpdateAcknowledgement(const StatusUpdate& update, const UPID& pid)
  {
    if (aborted) {
      VLOG(1) << "Not sending status update acknowledgment message because "
              << "the driver is aborted!";
      return;
    }

    StatusUpdateAcknowledgementMessage message;
    message.mutable_framework_id()->MergeFrom(framework.id());
    message.mutable_slave_id()->MergeFrom(update.slave_id());
    message.mutable_task_id()->MergeFrom(update.status().task_id());
    message.set_uuid(update.uuid());
    send(pid, message);
  }

  void lostSlave(const SlaveID& slaveId)
  {
    if (aborted) {
      VLOG(1) << "Ignoring lost slave message because the driver is aborted!";
      return;
    }

    if (from != master) {
      LOG(WARNING) << "Ignoring lost slave message from " << from
                   << "because it is not from the registered master ("
                   << master << ")";
      return;
    }

    VLOG(1) << "Lost slave " << slaveId;

    savedSlavePids.erase(slaveId);

    scheduler->slaveLost(driver, slaveId);
  }

  void frameworkMessage(const SlaveID& slaveId,
                        const FrameworkID& frameworkId,
                        const ExecutorID& executorId,
                        const string& data)
  {
    if (aborted) {
      VLOG(1) << "Ignoring framework message because the driver is aborted!";
      return;
    }

    VLOG(1) << "Received framework message";

    scheduler->frameworkMessage(driver, executorId, slaveId, data);
  }

  void error(const string& message)
  {
    if (aborted) {
      VLOG(1) << "Ignoring error message because the driver is aborted!";
      return;
    }

    VLOG(1) << "Got error '" << message << "'";

    driver->abort();

    scheduler->error(driver, message);
  }

  void stop(bool failover)
  {
    VLOG(1) << "Stopping framework '" << framework.id() << "'";

    // Whether or not we send an unregister message, we want to
    // terminate this process.
    terminate(self());

    if (connected && !failover) {
      UnregisterFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(framework.id());
      send(master, message);
    }

    Lock lock(mutex);
    pthread_cond_signal(cond);
  }

  // NOTE: This function informs the master to stop attempting to send
  // messages to this scheduler. The abort flag stops any already
  // enqueued messages or messages in flight from being handled. We
  // don't want to terminate the process because one might do a
  // MesosSchedulerDriver::stop later, which dispatches to
  // SchedulerProcess::stop.
  void abort()
  {
    VLOG(1) << "Aborting framework '" << framework.id() << "'";

    aborted = true;

    if (!connected) {
      VLOG(1) << "Not sending a deactivate message as master is disconnected";
      return;
    }

    DeactivateFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(framework.id());
    send(master, message);

    Lock lock(mutex);
    pthread_cond_signal(cond);
  }

  void killTask(const TaskID& taskId)
  {
    if (!connected) {
      VLOG(1) << "Ignoring kill task message as master is disconnected";
      return;
    }

    KillTaskMessage message;
    message.mutable_framework_id()->MergeFrom(framework.id());
    message.mutable_task_id()->MergeFrom(taskId);
    send(master, message);
  }

  void requestResources(const vector<Request>& requests)
  {
    if (!connected) {
      VLOG(1) << "Ignoring request resources message as master is disconnected";
      return;
    }

    ResourceRequestMessage message;
    message.mutable_framework_id()->MergeFrom(framework.id());
    foreach (const Request& request, requests) {
      message.add_requests()->MergeFrom(request);
    }
    send(master, message);
  }

  void launchTasks(const OfferID& offerId,
                   const vector<TaskInfo>& tasks,
                   const Filters& filters)
  {
    if (!connected) {
      VLOG(1) << "Ignoring launch tasks message as master is disconnected";
      // NOTE: Reply to the framework with TASK_LOST messages for each
      // task. This is a hack for now, to not let the scheduler
      // believe the tasks are forever in PENDING state, when actually
      // the master never received the launchTask message. Also,
      // realize that this hack doesn't capture the case when the
      // scheduler process sends it but the master never receives it
      // (message lost, master failover etc).  In the future, this
      // should be solved by the replicated log and timeouts.
      foreach (const TaskInfo& task, tasks) {
        StatusUpdate update;
        update.mutable_framework_id()->MergeFrom(framework.id());
        TaskStatus* status = update.mutable_status();
        status->mutable_task_id()->MergeFrom(task.task_id());
        status->set_state(TASK_LOST);
        status->set_message("Master Disconnected");
        update.set_timestamp(Clock::now());
        update.set_uuid(UUID::random().toBytes());

        statusUpdate(update, UPID());
      }
      return;
    }

    // Check that each TaskInfo has either an ExecutorInfo or a
    // CommandInfo but not both.
    foreach (const TaskInfo& task, tasks) {
      if (task.has_executor() == task.has_command()) {
        StatusUpdate update;
        update.mutable_framework_id()->MergeFrom(framework.id());
        TaskStatus* status = update.mutable_status();
        status->mutable_task_id()->MergeFrom(task.task_id());
        status->set_state(TASK_LOST);
        status->set_message(
            "TaskInfo must have either an 'executor' or a 'command'");
        update.set_timestamp(Clock::now());
        update.set_uuid(UUID::random().toBytes());

        statusUpdate(update, UPID());
      }
    }

    LaunchTasksMessage message;
    message.mutable_framework_id()->MergeFrom(framework.id());
    message.mutable_offer_id()->MergeFrom(offerId);
    message.mutable_filters()->MergeFrom(filters);

    foreach (const TaskInfo& task, tasks) {
      // Keep only the slave PIDs where we run tasks so we can send
      // framework messages directly.
      if (savedOffers.count(offerId) > 0) {
        if (savedOffers[offerId].count(task.slave_id()) > 0) {
          savedSlavePids[task.slave_id()] =
            savedOffers[offerId][task.slave_id()];
        } else {
          VLOG(1) << "Attempting to launch a task with the wrong slave id";
        }
      } else {
        VLOG(1) << "Attempting to launch a task with an unknown offer";
      }

      message.add_tasks()->MergeFrom(task);
    }

    // Remove the offer since we saved all the PIDs we might use.
    savedOffers.erase(offerId);

    send(master, message);
  }

  void reviveOffers()
  {
    if (!connected) {
      VLOG(1) << "Ignoring revive offers message as master is disconnected";
      return;
    }

    ReviveOffersMessage message;
    message.mutable_framework_id()->MergeFrom(framework.id());
    send(master, message);
  }

  void sendFrameworkMessage(const ExecutorID& executorId,
                            const SlaveID& slaveId,
                            const string& data)
  {
    if (!connected) {
     VLOG(1) << "Ignoring send framework message as master is disconnected";
     return;
    }

    VLOG(1) << "Asked to send framework message to slave "
            << slaveId;

    // TODO(benh): After a scheduler has re-registered it won't have
    // any saved slave PIDs, maybe it makes sense to try and save each
    // PID that this scheduler tries to send a message to? Or we can
    // just wait for them to recollect as new offers come in and get
    // accepted.

    if (savedSlavePids.count(slaveId) > 0) {
      UPID slave = savedSlavePids[slaveId];
      CHECK(slave != UPID());

      FrameworkToExecutorMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(framework.id());
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      send(slave, message);
    } else {
      VLOG(1) << "Cannot send directly to slave " << slaveId
              << "; sending through master";

      FrameworkToExecutorMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(framework.id());
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      send(master, message);
    }
  }

private:
  friend class mesos::MesosSchedulerDriver;

  MesosSchedulerDriver* driver;
  Scheduler* scheduler;
  FrameworkInfo framework;
  string url; // URL for the master (e.g., zk://, file://, etc).
  pthread_mutex_t* mutex;
  pthread_cond_t* cond;
  bool failover;
  UPID master;

  volatile bool connected; // Flag to indicate if framework is registered.
  volatile bool aborted; // Flag to indicate if the driver is aborted.

  Try<MasterDetector*> detector;

  hashmap<OfferID, hashmap<SlaveID, UPID> > savedOffers;
  hashmap<SlaveID, UPID> savedSlavePids;
};

} // namespace internal {
} // namespace mesos {


// Implementation of C++ API.
//
// Notes:
//
// (1) Callbacks should be serialized as well as calls into the
//     class. We do the former because the message reads from
//     SchedulerProcess are serialized. We do the latter currently by
//     using locks for certain methods ... but this may change in the
//     future.
//
// (2) There is a variable called state, that represents the current
//     state of the driver and is used to enforce its state transitions.

MesosSchedulerDriver::MesosSchedulerDriver(
    Scheduler* _scheduler,
    const FrameworkInfo& _framework,
    const string& _master)
  : scheduler(_scheduler),
    framework(_framework),
    master(_master),
    process(NULL),
    status(DRIVER_NOT_STARTED)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // Load the configuration. For now, we just load all key/value pairs
  // from the environment (and possibly a file if specified) but don't
  // actually do any validation on them (since we don't register any
  // options). Any "validation" necessary will be done when we load
  // the configuration into flags (i.e., below when we initialize
  // logging or inside of local::launch).
  Configurator configurator;
  Configuration configuration;
  try {
    configuration = configurator.load();
  } catch (ConfigurationException& e) {
    status = DRIVER_ABORTED;
    string message = string("Configuration error: ") + e.what();
    scheduler->error(this, message);
    return;
  }

  flags::Flags<logging::Flags> flags;

  flags.load(configuration.getMap());

  // Initialize libprocess.
  process::initialize();

  // TODO(benh): Consider eliminating 'localquiet' so that we don't
  // have to have weird semantics when the 'quiet' option is set to
  // false but 'localquiet' is being used.
  configuration.set("quiet", master == "localquiet");

  // TODO(benh): Replace whitespace in framework.name() with '_'?
  logging::initialize(framework.name(), flags);

  // Initialize mutex and condition variable. TODO(benh): Consider
  // using a libprocess Latch rather than a pthread mutex and
  // condition variable for signaling.
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  pthread_cond_init(&cond, 0);

  // TODO(benh): Check the user the framework wants to run tasks as,
  // see if the current user can switch to that user, or via an
  // authentication module ensure this is acceptable.

  // If no user specified, just use the current user.
  if (framework.user() == "") {
    framework.set_user(os::user());
  }

  // Launch a local cluster if necessary.
  Option<UPID> pid;
  if (master == "local" || master == "localquiet") {
    pid = local::launch(configuration);
  }

  CHECK(process == NULL);

  if (pid.isSome()) {
    process = new SchedulerProcess(
        this, scheduler, framework, pid.get(), &mutex, &cond);
  } else {
    process = new SchedulerProcess(
        this, scheduler, framework, master, &mutex, &cond);
  }
}


MesosSchedulerDriver::~MesosSchedulerDriver()
{
  // We want to make sure the SchedulerProcess has completed so it
  // doesn't try to make calls into us after we are gone. There is an
  // unfortunate deadlock scenario that occurs when we try and wait
  // for a process that we are currently executing within (e.g.,
  // because a callback on 'this' invoked from a SchedulerProcess
  // ultimately invokes this destructor). This deadlock is actually a
  // bug in the client code: provided that the SchedulerProcess class
  // _only_ makes calls into instances of Scheduler, then such a
  // deadlock implies that the destructor got called from within a
  // method of the Scheduler instance that is being destructed! Note
  // that we could add a method to libprocess that told us whether or
  // not this was about to be deadlock, and possibly report this back
  // to the user somehow. Note that we will also wait forever if
  // MesosSchedulerDriver::stop was never called. It might make sense
  // to try and add some more debug output for the case where we wait
  // indefinitely due to deadlock ...
  if (process != NULL) {
    wait(process);
    delete process;
  }

  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);

  // Check and see if we need to shutdown a local cluster.
  if (master == "local" || master == "localquiet") {
    local::shutdown();
  }
}


Status MesosSchedulerDriver::start()
{
  Lock lock(&mutex);

  if (status != DRIVER_NOT_STARTED) {
    return status;
  }

  CHECK(process != NULL);

  spawn(process);

  return status = DRIVER_RUNNING;
}


Status MesosSchedulerDriver::stop(bool failover)
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING && status != DRIVER_ABORTED) {
    return status;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::stop, failover);

  // TODO: It might make more sense to clean up our local cluster here than in
  // the destructor. However, what would be even better is to allow multiple
  // local clusters to exist (i.e. not use global vars in local.cpp) so that
  // ours can just be an instance variable in MesosSchedulerDriver.

  bool aborted = status == DRIVER_ABORTED;

  status = DRIVER_STOPPED;

  return aborted ? DRIVER_ABORTED : status;
}


Status MesosSchedulerDriver::abort()
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::abort);

  return status = DRIVER_ABORTED;
}


Status MesosSchedulerDriver::join()
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  while (status == DRIVER_RUNNING) {
    pthread_cond_wait(&cond, &mutex);
  }

  CHECK(status == DRIVER_ABORTED || status == DRIVER_STOPPED);

  return status;
}


Status MesosSchedulerDriver::run()
{
  Status status = start();
  return status != DRIVER_RUNNING ? status : join();
}


Status MesosSchedulerDriver::killTask(const TaskID& taskId)
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::killTask, taskId);

  return status;
}


Status MesosSchedulerDriver::launchTasks(
    const OfferID& offerId,
    const vector<TaskInfo>& tasks,
    const Filters& filters)
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::launchTasks, offerId, tasks, filters);

  return status;
}


Status MesosSchedulerDriver::declineOffer(
    const OfferID& offerId,
    const Filters& filters)
{
  return launchTasks(offerId, vector<TaskInfo>(), filters);
}


Status MesosSchedulerDriver::reviveOffers()
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::reviveOffers);

  return status;
}


Status MesosSchedulerDriver::sendFrameworkMessage(
    const ExecutorID& executorId,
    const SlaveID& slaveId,
    const string& data)
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::sendFrameworkMessage,
           executorId, slaveId, data);

  return status;
}


Status MesosSchedulerDriver::requestResources(
    const vector<Request>& requests)
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::requestResources, requests);

  return status;
}
