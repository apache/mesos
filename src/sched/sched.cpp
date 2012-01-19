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

#include <tr1/functional>

#include <mesos/scheduler.hpp>

#include <process/dispatch.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/timer.hpp>

#include "configurator/configuration.hpp"

#include "common/fatal.hpp"
#include "common/hashmap.hpp"
#include "common/lock.hpp"
#include "common/logging.hpp"
#include "common/type_utils.hpp"
#include "common/uuid.hpp"

#include "detector/detector.hpp"

#include "local/local.hpp"

#include "messages/messages.hpp"

using namespace mesos;
using namespace mesos::internal;

using namespace process;

using std::map;
using std::string;
using std::vector;

using process::wait; // Necessary on some OS's to disambiguate.

using std::tr1::bind;


namespace mesos { namespace internal {

// The scheduler process (below) is responsible for interacting with
// the master and responding to Mesos API calls from scheduler
// drivers. In order to allow a message to be sent back to the master
// we allow friend functions to invoke 'send', 'post', etc. Therefore,
// we must make sure that any necessary synchronization is performed.

class SchedulerProcess : public ProtobufProcess<SchedulerProcess>
{
public:
  SchedulerProcess(MesosSchedulerDriver* _driver,
                   Scheduler* _sched,
                   const FrameworkID& _frameworkId,
                   const FrameworkInfo& _framework)
    : driver(_driver),
      sched(_sched),
      frameworkId(_frameworkId),
      framework(_framework),
      master(UPID()),
      failover(!(_frameworkId == "")),
      connected(false),
      aborted(false)
  {
    installProtobufHandler<NewMasterDetectedMessage>(
        &SchedulerProcess::newMasterDetected,
        &NewMasterDetectedMessage::pid);

    installProtobufHandler<NoMasterDetectedMessage>(
        &SchedulerProcess::noMasterDetected);

    installProtobufHandler<FrameworkRegisteredMessage>(
        &SchedulerProcess::registered,
        &FrameworkRegisteredMessage::framework_id);

    installProtobufHandler<FrameworkReregisteredMessage>(
        &SchedulerProcess::reregistered,
        &FrameworkReregisteredMessage::framework_id);

    installProtobufHandler<ResourceOffersMessage>(
        &SchedulerProcess::resourceOffers,
        &ResourceOffersMessage::offers,
        &ResourceOffersMessage::pids);

    installProtobufHandler<RescindResourceOfferMessage>(
        &SchedulerProcess::rescindOffer,
        &RescindResourceOfferMessage::offer_id);

    installProtobufHandler<StatusUpdateMessage>(
        &SchedulerProcess::statusUpdate,
        &StatusUpdateMessage::update,
        &StatusUpdateMessage::pid);

    installProtobufHandler<LostSlaveMessage>(
        &SchedulerProcess::lostSlave,
        &LostSlaveMessage::slave_id);

    installProtobufHandler<ExecutorToFrameworkMessage>(
        &SchedulerProcess::frameworkMessage,
        &ExecutorToFrameworkMessage::slave_id,
        &ExecutorToFrameworkMessage::framework_id,
        &ExecutorToFrameworkMessage::executor_id,
        &ExecutorToFrameworkMessage::data);

    installProtobufHandler<FrameworkErrorMessage>(
        &SchedulerProcess::error,
        &FrameworkErrorMessage::code,
        &FrameworkErrorMessage::message);
  }

  virtual ~SchedulerProcess() {}

protected:
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
  }

  void registered(const FrameworkID& frameworkId)
  {
    if (aborted) {
      VLOG(1) << "Ignoring framework registered message because "
              << "the driver is aborted!";
      return;
    }

    VLOG(1) << "Framework registered with " << frameworkId;

    this->frameworkId = frameworkId;
    connected = true;
    failover = false;

    invoke(bind(&Scheduler::registered, sched, driver, frameworkId));
  }

  void reregistered(const FrameworkID& frameworkId)
  {
    VLOG(1) << "Framework re-registered with " << frameworkId;
    CHECK(this->frameworkId == frameworkId);

    connected = true;
    failover = false;
  }

  void doReliableRegistration()
  {
    if (connected || !master) {
      return;
    }

    if (frameworkId == "") {
      // Touched for the very first time.
      RegisterFrameworkMessage message;
      message.mutable_framework()->MergeFrom(framework);
      send(master, message);
    } else {
      // Not the first time, or failing over.
      ReregisterFrameworkMessage message;
      message.mutable_framework()->MergeFrom(framework);
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.set_failover(failover);
      send(master, message);
    }

    delay(1.0, self(), &SchedulerProcess::doReliableRegistration);
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
    for (int i = 0; i < offers.size(); i++) {
      UPID pid(pids[i]);
      // Check if parse failed (e.g., due to DNS).
      if (pid != UPID()) {
        VLOG(2) << "Saving PID '" << pids[i] << "'";
        savedOffers[offers[i].id()][offers[i].slave_id()] = pid;
      } else {
        VLOG(2) << "Failed to parse PID '" << pids[i] << "'";
      }
    }

    invoke(bind(&Scheduler::resourceOffers, sched, driver, offers));
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
    invoke(bind(&Scheduler::offerRescinded, sched, driver, offerId));
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

    CHECK(frameworkId == update.framework_id());

    // TODO(benh): Note that this maybe a duplicate status update!
    // Once we get support to try and have a more consistent view
    // of what's running in the cluster, we'll just let this one
    // slide. The alternative is possibly dealing with a scheduler
    // failover and not correctly giving the scheduler it's status
    // update, which seems worse than giving a status update
    // multiple times (of course, if a scheduler re-uses a TaskID,
    // that could be bad.

    invoke(bind(&Scheduler::statusUpdate, sched, driver, status));

    // Send a status update acknowledgement ONLY if not aborted!
    if (!aborted && pid) {
      // Acknowledge the message (we do this last, after we invoked
      // the scheduler, if we did at all, in case it causes a crash,
      // since this way the message might get resent/routed after the
      // scheduler comes back online).
      StatusUpdateAcknowledgementMessage message;
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_slave_id()->MergeFrom(update.slave_id());
      message.mutable_task_id()->MergeFrom(status.task_id());
      message.set_uuid(update.uuid());
      send(pid, message);
    }
  }

  void lostSlave(const SlaveID& slaveId)
  {
    if (aborted) {
      VLOG(1) << "Ignoring lost slave message because the driver is aborted!";
      return;
    }

    VLOG(1) << "Lost slave " << slaveId;

    savedSlavePids.erase(slaveId);
    invoke(bind(&Scheduler::slaveLost, sched, driver, slaveId));
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

    invoke(bind(&Scheduler::frameworkMessage,
                sched, driver, slaveId, executorId, data));
  }

  void error(int32_t code, const string& message)
  {
    if (aborted) {
      VLOG(1) << "Ignoring error message because the driver is aborted!";
      return;
    }

    VLOG(1) << "Got error '" << message << "' (code: " << code << ")";

    driver->abort();

    invoke(bind(&Scheduler::error, sched, driver, code, message));
  }

  void stop(bool failover)
  {
    VLOG(1) << "Stopping the framework";

    // Whether or not we send an unregister message, we want to
    // terminate this process.
    terminate(self());

    if (connected && !failover) {
      UnregisterFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(frameworkId);
      send(master, message);
    }
  }

  // NOTE: This function stops any further callbacks from reaching the
  // scheduler by informing the master. The abort flag stops
  // those callbacks that are already enqueued.
  void abort()
  {
    VLOG(1) << "Aborting the framework";
    aborted = true;

    if (!connected) {
      VLOG(1) << "Not sending a deactivate message as master is disconnected";
      return;
    }

    VLOG(1) << "Deactivating the framework " << frameworkId;

    DeactivateFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    send(master, message);

  }

  void killTask(const TaskID& taskId)
  {
    if (!connected) {
      VLOG(1) << "Ignoring kill task message as master is disconnected";
      return;
    }

    KillTaskMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_task_id()->MergeFrom(taskId);
    send(master, message);
  }

  void requestResources(const vector<ResourceRequest>& requests)
  {
    if (!connected) {
      VLOG(1) << "Ignoring request resources message as master is disconnected";
      return;
    }

    ResourceRequestMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    foreach (const ResourceRequest& request, requests) {
      message.add_requests()->MergeFrom(request);
    }
    send(master, message);
  }

  void launchTasks(const OfferID& offerId,
                   const vector<TaskDescription>& tasks,
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
      foreach (const TaskDescription& task, tasks) {
        StatusUpdate update;
        update.mutable_framework_id()->MergeFrom(frameworkId);
        TaskStatus* status = update.mutable_status();
        status->mutable_task_id()->MergeFrom(task.task_id());
        status->set_state(TASK_LOST);
        status->set_message("Master Disconnected");
        update.set_timestamp(elapsedTime());
        update.set_uuid(UUID::random().toBytes());

        statusUpdate(update, UPID());
      }
      return;
    }

    LaunchTasksMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_offer_id()->MergeFrom(offerId);
    message.mutable_filters()->MergeFrom(filters);

    foreach (const TaskDescription& task, tasks) {
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
    message.mutable_framework_id()->MergeFrom(frameworkId);
    send(master, message);
  }

  void sendFrameworkMessage(const SlaveID& slaveId,
                            const ExecutorID& executorId,
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
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      send(slave, message);
    } else {
      VLOG(1) << "Cannot send directly to slave " << slaveId
	      << "; sending through master";

      FrameworkToExecutorMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      send(master, message);
    }
  }

private:
  friend class mesos::MesosSchedulerDriver;

  MesosSchedulerDriver* driver;
  Scheduler* sched;
  FrameworkID frameworkId;
  FrameworkInfo framework;
  bool failover;
  UPID master;

  volatile bool connected; // Flag to indicate if framework is registered.
  volatile bool aborted; // Flag to indicate if the driver is aborted.

  hashmap<OfferID, hashmap<SlaveID, UPID> > savedOffers;
  hashmap<SlaveID, UPID> savedSlavePids;
};

}} // namespace mesos { namespace internal {


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

MesosSchedulerDriver::MesosSchedulerDriver(Scheduler* sched,
                                           const std::string& frameworkName,
                                           const ExecutorInfo& executorInfo,
                                           const string& url,
                                           const FrameworkID& frameworkId)
{
  Configurator configurator;
  // TODO(benh): Only register local options if this is running with
  // 'local' or 'localquiet'! Perhaps create a registerOptions for the
  // scheduler?
  local::registerOptions(&configurator);
  Configuration* conf;
  try {
    conf = new Configuration(configurator.load());
  } catch (ConfigurationException& e) {
    // TODO(benh|matei): Are error callbacks not fatal!?
    string message = string("Configuration error: ") + e.what();
    sched->error(this, 2, message);
    conf = new Configuration();
  }
  conf->set("url", url); // Override URL param with the one from the user
  init(sched, conf, frameworkId, frameworkName, executorInfo);
}


MesosSchedulerDriver::MesosSchedulerDriver(Scheduler* sched,
                                           const std::string& frameworkName,
                                           const ExecutorInfo& executorInfo,
                                           const map<string, string> &params,
                                           const FrameworkID& frameworkId)
{
  Configurator configurator;
  // TODO(benh): Only register local options if this is running with
  // 'local' or 'localquiet'! Perhaps create a registerOptions for the
  // scheduler?
  local::registerOptions(&configurator);
  Configuration* conf;
  try {
    conf = new Configuration(configurator.load(params));
  } catch (ConfigurationException& e) {
    // TODO(benh|matei): Are error callbacks not fatal?
    string message = string("Configuration error: ") + e.what();
    sched->error(this, 2, message);
    conf = new Configuration();
  }
  init(sched, conf, frameworkId, frameworkName, executorInfo);
}


MesosSchedulerDriver::MesosSchedulerDriver(Scheduler* sched,
                                           const std::string& frameworkName,
                                           const ExecutorInfo& executorInfo,
                                           int argc,
                                           char** argv,
                                           const FrameworkID& frameworkId)
{
  Configurator configurator;
  // TODO(benh): Only register local options if this is running with
  // 'local' or 'localquiet'! Perhaps create a registerOptions for the
  // scheduler?
  local::registerOptions(&configurator);
  Configuration* conf;
  try {
    conf = new Configuration(configurator.load(argc, argv, false));
  } catch (ConfigurationException& e) {
    string message = string("Configuration error: ") + e.what();
    sched->error(this, 2, message);
    conf = new Configuration();
  }
  init(sched, conf, frameworkId, frameworkName, executorInfo);
}


void MesosSchedulerDriver::init(Scheduler* _sched,
                                Configuration* _conf,
                                const FrameworkID& _frameworkId,
                                const std::string& _frameworkName,
                                const ExecutorInfo& _executorInfo)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  sched = _sched;
  conf = _conf;
  frameworkId = _frameworkId;
  frameworkName = _frameworkName;
  executorInfo = _executorInfo;
  url = conf->get<string>("url", "local");
  process = NULL;
  detector = NULL;
  state = INITIALIZED;

  // Create mutex and condition variable
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  pthread_cond_init(&cond, 0);

  // TODO(benh): Initialize glog.

  // Initialize libprocess library (but not glog, done above).
  process::initialize(false);
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
  // MesosSchedulerDriver::stop was never called.
  if (process != NULL) {
    wait(process);
    delete process;
  }

  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);

  if (detector != NULL) {
    MasterDetector::destroy(detector);
  }

  // Delete conf since we always create it ourselves with new
  delete conf;

  // Check and see if we need to shutdown a local cluster.
  if (url == "local" || url == "localquiet") {
    local::shutdown();
  }
}


Status MesosSchedulerDriver::start()
{
  Lock lock(&mutex);

  if (state == RUNNING) {
    return DRIVER_ALREADY_RUNNING;
  } else if (state == STOPPED) {
    return DRIVER_STOPPED;
  } else if (state == ABORTED) {
    return DRIVER_ABORTED;
  }

  // Set running here so we can recognize an exception from calls into
  // Java (via getFrameworkName or getExecutorInfo).
  state = RUNNING;;

  // Set up framework info.
  FrameworkInfo framework;
  framework.set_user(utils::os::user());
  framework.set_name(frameworkName);
  framework.mutable_executor()->MergeFrom(executorInfo);

  CHECK(process == NULL);

  process = new SchedulerProcess(this, sched, frameworkId, framework);

  UPID pid = spawn(process);

  // Check and see if we need to launch a local cluster.
  if (url == "local") {
    const PID<master::Master>& master = local::launch(*conf);
    detector = new BasicMasterDetector(master, pid);
  } else if (url == "localquiet") {
    conf->set("quiet", 1);
    const PID<master::Master>& master = local::launch(*conf);
    detector = new BasicMasterDetector(master, pid);
  } else {
    detector = MasterDetector::create(url, pid, false, false);
  }

  return OK;
}


Status MesosSchedulerDriver::stop(bool failover)
{
  Lock lock(&mutex);

  if (state == STOPPED) {
    return DRIVER_STOPPED;
  } else if (state != RUNNING && state != ABORTED) {
    return DRIVER_NOT_RUNNING;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::stop, failover);

  state = STOPPED;

  // TODO: It might make more sense to clean up our local cluster here than in
  // the destructor. However, what would be even better is to allow multiple
  // local clusters to exist (i.e. not use global vars in local.cpp) so that
  // ours can just be an instance variable in MesosSchedulerDriver.

  pthread_cond_signal(&cond);

  return OK;
}


Status MesosSchedulerDriver::abort()
{
  Lock lock(&mutex);

  if (state == ABORTED) {
    return DRIVER_ABORTED;
  } else if (state == STOPPED) {
    return DRIVER_STOPPED;
  } else if (state != RUNNING) {
    return DRIVER_NOT_RUNNING;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::abort);

  state = ABORTED;

  pthread_cond_signal(&cond);

  return OK;
}


Status MesosSchedulerDriver::join()
{
  Lock lock(&mutex);

  if (state == ABORTED) {
    return DRIVER_ABORTED;
  } else if (state == STOPPED) {
    return DRIVER_STOPPED;
  } else if (state != RUNNING) {
    return DRIVER_NOT_RUNNING;
  }

  while (state == RUNNING) {
    pthread_cond_wait(&cond, &mutex);
  }

  if (state == ABORTED)
    return DRIVER_ABORTED;

  CHECK(state == STOPPED);

  return OK;
}


Status MesosSchedulerDriver::run()
{
  Status status = start();
  return status != OK ? status : join();
}


Status MesosSchedulerDriver::killTask(const TaskID& taskId)
{
  Lock lock(&mutex);

  if (state == ABORTED) {
    return DRIVER_ABORTED;
  } else if (state != RUNNING) {
    return DRIVER_NOT_RUNNING;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::killTask, taskId);

  return OK;
}


Status MesosSchedulerDriver::launchTasks(const OfferID& offerId,
                                         const vector<TaskDescription>& tasks,
                                         const Filters& filters)
{
  Lock lock(&mutex);

  if (state == ABORTED) {
    return DRIVER_ABORTED;
  } else if (state != RUNNING) {
    return DRIVER_NOT_RUNNING;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::launchTasks, offerId, tasks, filters);

  return OK;
}


Status MesosSchedulerDriver::reviveOffers()
{
  Lock lock(&mutex);

  if (state == ABORTED) {
    return DRIVER_ABORTED;
  } else if (state != RUNNING) {
    return DRIVER_NOT_RUNNING;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::reviveOffers);

  return OK;
}


Status MesosSchedulerDriver::sendFrameworkMessage(
    const SlaveID& slaveId,
    const ExecutorID& executorId,
    const string& data)
{
  Lock lock(&mutex);

  if (state == ABORTED) {
    return DRIVER_ABORTED;
  } else if (state != RUNNING) {
    return DRIVER_NOT_RUNNING;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::sendFrameworkMessage,
           slaveId, executorId, data);

  return OK;
}


void MesosSchedulerDriver::error(int code, const string& message)
{
  sched->error(this, code, message);
}


Status MesosSchedulerDriver::requestResources(
    const vector<ResourceRequest>& requests)
{
  Lock lock(&mutex);

  if (state == ABORTED) {
    return DRIVER_ABORTED;
  } else if (state != RUNNING) {
    return DRIVER_NOT_RUNNING;
  }

  CHECK(process != NULL);

  dispatch(process, &SchedulerProcess::requestResources, requests);

  return OK;
}
