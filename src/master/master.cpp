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

#include <fstream>
#include <iomanip>
#include <sstream>

#include <glog/logging.h>

#include <process/run.hpp>
#include <process/timer.hpp>

#include "config/config.hpp"

#include "common/build.hpp"
#include "common/date_utils.hpp"
#include "common/utils.hpp"
#include "common/uuid.hpp"

#include "allocator.hpp"
#include "allocator_factory.hpp"
#include "master.hpp"
#include "slaves_manager.hpp"
#include "webui.hpp"

using std::string;
using std::vector;

using process::wait; // Necessary on some OS's to disambiguate.


namespace mesos { namespace internal { namespace master {

class SlaveObserver : public Process<SlaveObserver>
{
public:
  SlaveObserver(const UPID& _slave,
                const SlaveInfo& _slaveInfo,
                const SlaveID& _slaveId,
                const PID<SlavesManager>& _slavesManager)
    : slave(_slave),
      slaveInfo(_slaveInfo),
      slaveId(_slaveId),
      slavesManager(_slavesManager),
      timeouts(0),
      pinged(false) {}

  virtual ~SlaveObserver() {}

protected:
  virtual void operator () ()
  {
    // Send a ping some interval after we heard the last pong. Or if
    // we don't hear a pong, increment the number of timeouts from the
    // slave and try and send another ping. If we eventually timeout too
    // many missed pongs in a row, consider the slave dead.
    send(slave, "PING");
    pinged = true;

    do {
      receive(SLAVE_PONG_TIMEOUT);
      if (name() == "PONG") {
        timeouts = 0;
        pinged = false;
      } else if (name() == TIMEOUT) {
        if (pinged) {
          timeouts++;
          pinged = false;
        }

        send(slave, "PING");
        pinged = true;
      } else if (name() == TERMINATE) {
        return;
      } 
    } while (timeouts < MAX_SLAVE_TIMEOUTS);

    // Tell the slave manager to deactivate the slave, this will take
    // care of updating the master too.
    while (!call(slavesManager, &SlavesManager::deactivate,
                 slaveInfo.hostname(), slave.port)) {
      LOG(WARNING) << "Slave \"failed\" but can't be deactivated, retrying";
      pause(5);
    }
  }

private:
  const UPID slave;
  const SlaveInfo slaveInfo;
  const SlaveID slaveId;
  const PID<SlavesManager> slavesManager;
  int timeouts;
  bool pinged;
};


// Performs slave registration asynchronously. There are two means of
// doing this, one first tries to add this slave to the slaves
// manager, while the other one simply tells the master to add the
// slave.
struct SlaveRegistrar
{
  static bool run(Slave* slave, const PID<Master>& master)
  {
    // TODO(benh): Do a reverse lookup to ensure IP maps to
    // hostname, or check credentials of this slave.
    dispatch(master, &Master::addSlave, slave, false);
  }

  static bool run(Slave* slave,
                  const PID<Master>& master,
                  const PID<SlavesManager>& slavesManager)
  {
    if (!call(slavesManager, &SlavesManager::add,
              slave->info.hostname(), slave->pid.port)) {
      LOG(WARNING) << "Could not register slave because failed"
                   << " to add it to the slaves maanger";
      delete slave;
      return false;
    }

    return run(slave, master);
  }
};


// Performs slave re-registration asynchronously as above.
struct SlaveReregistrar
{
  static bool run(Slave* slave,
                  const vector<Task>& tasks,
                  const PID<Master>& master)
  {
    // TODO(benh): Do a reverse lookup to ensure IP maps to
    // hostname, or check credentials of this slave.
    dispatch(master, &Master::readdSlave, slave, tasks);
  }

  static bool run(Slave* slave,
                  const vector<Task>& tasks,
                  const PID<Master>& master,
                  const PID<SlavesManager>& slavesManager)
  {
    if (!call(slavesManager, &SlavesManager::add,
              slave->info.hostname(), slave->pid.port)) {
      LOG(WARNING) << "Could not register slave because failed"
                   << " to add it to the slaves maanger";
      delete slave;
      return false;
    }

    return run(slave, tasks, master);
  }
};


Master::Master()
  : ProcessBase("master")
{
  initialize();
}


Master::Master(const Configuration& conf)
  : ProcessBase("master"),
    conf(conf)
{
  initialize();
}
                   

Master::~Master()
{
  LOG(INFO) << "Shutting down master";

  foreachvalue (Framework* framework, utils::copy(frameworks)) {
    removeFramework(framework);
  }

  foreachvalue (Slave* slave, utils::copy(slaves)) {
    removeSlave(slave);
  }

  CHECK(offers.size() == 0);

  terminate(slavesManager);
  wait(slavesManager);

  delete slavesManager;

  delete allocator;
}


void Master::registerOptions(Configurator* configurator)
{
  SlavesManager::registerOptions(configurator);

  configurator->addOption<string>(
      "allocator",
      'a',
      "Allocation module name",
      "simple");

  configurator->addOption<bool>(
      "root_submissions",
      "Can root submit frameworks?",
      true);
}


Promise<state::MasterState*> Master::getState()
{
  state::MasterState* state =
    new state::MasterState(build::DATE, build::USER, self());

  foreachvalue (Slave* s, slaves) {
    Resources resources(s->info.resources());
    Resource::Scalar cpus;
    Resource::Scalar mem;
    cpus.set_value(0);
    mem.set_value(0);
    cpus = resources.getScalar("cpus", cpus);
    mem = resources.getScalar("mem", mem);

    state::Slave* slave =
      new state::Slave(s->id.value(), s->info.hostname(),
                       s->info.public_hostname(), cpus.value(),
                       mem.value(), s->registeredTime);

    state->slaves.push_back(slave);
  }

  foreachvalue (Framework* f, frameworks) {
    Resources resources(f->resources);
    Resource::Scalar cpus;
    Resource::Scalar mem;
    cpus.set_value(0);
    mem.set_value(0);
    cpus = resources.getScalar("cpus", cpus);
    mem = resources.getScalar("mem", mem);

    state::Framework* framework =
      new state::Framework(f->id.value(), f->info.user(),
                           f->info.name(), f->info.executor().uri(),
                           cpus.value(), mem.value(), f->registeredTime);

    state->frameworks.push_back(framework);

    foreachvalue (Task* t, f->tasks) {
      Resources resources(t->resources());
      Resource::Scalar cpus;
      Resource::Scalar mem;
      cpus.set_value(0);
      mem.set_value(0);
      cpus = resources.getScalar("cpus", cpus);
      mem = resources.getScalar("mem", mem);

      state::Task* task =
        new state::Task(t->task_id().value(), t->name(),
                        t->framework_id().value(), t->slave_id().value(),
                        TaskState_descriptor()->FindValueByNumber(t->state())->name(),
                        cpus.value(), mem.value());

      framework->tasks.push_back(task);
    }

    foreach (Offer* o, f->offers) {
      state::Offer* offer =
        new state::Offer(o->id.value(), o->frameworkId.value());

      foreach (const SlaveResources& r, o->resources) {
        Resources resources(r.resources);
        Resource::Scalar cpus;
        Resource::Scalar mem;
        cpus.set_value(0);
        mem.set_value(0);
        cpus = resources.getScalar("cpus", cpus);
        mem = resources.getScalar("mem", mem);

        state::SlaveResources* sr =
          new state::SlaveResources(r.slave->id.value(),
                                    cpus.value(), mem.value());

        offer->resources.push_back(sr);
      }

      framework->offers.push_back(offer);
    }
  }
  
  return state;
}


// Return connected frameworks that are not in the process of being removed
vector<Framework*> Master::getActiveFrameworks()
{
  vector <Framework*> result;
  foreachvalue (Framework* framework, frameworks) {
    if (framework->active) {
      result.push_back(framework);
    }
  }
  return result;
}


// Return connected slaves that are not in the process of being removed
vector<Slave*> Master::getActiveSlaves()
{
  vector <Slave*> result;
  foreachvalue (Slave* slave, slaves) {
    if (slave->active) {
      result.push_back(slave);
    }
  }
  return result;
}


void Master::operator () ()
{
  LOG(INFO) << "Master started at mesos://" << self();

  // Don't do anything until we get a master token.
  while (receive() != GotMasterTokenMessage().GetTypeName()) {
    LOG(INFO) << "Oops! We're dropping a message since "
              << "we haven't received an identifier yet!";  
  }

  GotMasterTokenMessage message;
  message.ParseFromString(body());

  // The master ID is comprised of the current date and some ephemeral
  // token (e.g., determined by ZooKeeper).

  masterId = DateUtils::currentDate() + "-" + message.token();
  LOG(INFO) << "Master ID: " << masterId;

  // Setup slave manager.
  slavesManager = new SlavesManager(conf, self());
  spawn(slavesManager);

  // Create the allocator (we do this after the constructor because it
  // leaks 'this').
  string type = conf.get("allocator", "simple");
  LOG(INFO) << "Creating \"" << type << "\" allocator";
  allocator = AllocatorFactory::instantiate(type, this);

  if (!allocator) {
    LOG(FATAL) << "Unrecognized allocator type: " << type;
  }

  // Start our timer ticks.
  delay(1.0, self(), &Master::timerTick);

  while (true) {
    serve();
    if (name() == TERMINATE) {
      LOG(INFO) << "Asked to terminate by " << from();
      foreachvalue (Slave* slave, slaves) {
        send(slave->pid, TERMINATE);
      }
      break;
    } else {
      LOG(WARNING) << "Dropping unknown message '" << name() << "'"
                   << " from: " << from();
    }
  }
}


void Master::initialize()
{
  active = false;

  nextFrameworkId = 0;
  nextSlaveId = 0;
  nextOfferId = 0;

  // Start all the statistics at 0.
  CHECK(TASK_STARTING == TaskState_MIN);
  CHECK(TASK_LOST == TaskState_MAX);
  stats.tasks[TASK_STARTING] = 0;
  stats.tasks[TASK_RUNNING] = 0;
  stats.tasks[TASK_FINISHED] = 0;
  stats.tasks[TASK_FAILED] = 0;
  stats.tasks[TASK_KILLED] = 0;
  stats.tasks[TASK_LOST] = 0;
  stats.validStatusUpdates = 0;
  stats.invalidStatusUpdates = 0;
  stats.validFrameworkMessages = 0;
  stats.invalidFrameworkMessages = 0;

  startTime = elapsedTime();

  // Install handler functions for certain messages.
  installProtobufHandler<NewMasterDetectedMessage>(
      &Master::newMasterDetected,
      &NewMasterDetectedMessage::pid);

  installProtobufHandler<NoMasterDetectedMessage>(
      &Master::noMasterDetected);

  installProtobufHandler<RegisterFrameworkMessage>(
      &Master::registerFramework,
      &RegisterFrameworkMessage::framework);

  installProtobufHandler<ReregisterFrameworkMessage>(
      &Master::reregisterFramework,
      &ReregisterFrameworkMessage::framework_id,
      &ReregisterFrameworkMessage::framework,
      &ReregisterFrameworkMessage::generation);

  installProtobufHandler<UnregisterFrameworkMessage>(
      &Master::unregisterFramework,
      &UnregisterFrameworkMessage::framework_id);

  installProtobufHandler<ResourceOfferReplyMessage>(
      &Master::resourceOfferReply,
      &ResourceOfferReplyMessage::framework_id,
      &ResourceOfferReplyMessage::offer_id,
      &ResourceOfferReplyMessage::tasks,
      &ResourceOfferReplyMessage::params);

  installProtobufHandler<ReviveOffersMessage>(
      &Master::reviveOffers,
      &ReviveOffersMessage::framework_id);

  installProtobufHandler<KillTaskMessage>(
      &Master::killTask,
      &KillTaskMessage::framework_id,
      &KillTaskMessage::task_id);

  installProtobufHandler<FrameworkToExecutorMessage>(
      &Master::schedulerMessage,
      &FrameworkToExecutorMessage::slave_id,
      &FrameworkToExecutorMessage::framework_id,
      &FrameworkToExecutorMessage::executor_id,
      &FrameworkToExecutorMessage::data);

  installProtobufHandler<RegisterSlaveMessage>(
      &Master::registerSlave,
      &RegisterSlaveMessage::slave);

  installProtobufHandler<ReregisterSlaveMessage>(
      &Master::reregisterSlave,
      &ReregisterSlaveMessage::slave_id,
      &ReregisterSlaveMessage::slave,
      &ReregisterSlaveMessage::tasks);

  installProtobufHandler<UnregisterSlaveMessage>(
      &Master::unregisterSlave,
      &UnregisterSlaveMessage::slave_id);

  installProtobufHandler<StatusUpdateMessage>(
      &Master::statusUpdate,
      &StatusUpdateMessage::update,
      &StatusUpdateMessage::pid);

  installProtobufHandler<ExecutorToFrameworkMessage>(
      &Master::executorMessage,
      &ExecutorToFrameworkMessage::slave_id,
      &ExecutorToFrameworkMessage::framework_id,
      &ExecutorToFrameworkMessage::executor_id,
      &ExecutorToFrameworkMessage::data);

  installProtobufHandler<ExitedExecutorMessage>(
      &Master::exitedExecutor,
      &ExitedExecutorMessage::slave_id,
      &ExitedExecutorMessage::framework_id,
      &ExitedExecutorMessage::executor_id,
      &ExitedExecutorMessage::status);

  // Install some message handlers.
  installMessageHandler(EXITED, &Master::exited);

  // Install HTTP request handlers.
  installHttpHandler("info.json", &Master::http_info_json);
  installHttpHandler("frameworks.json", &Master::http_frameworks_json);
  installHttpHandler("slaves.json", &Master::http_slaves_json);
  installHttpHandler("tasks.json", &Master::http_tasks_json);
  installHttpHandler("stats.json", &Master::http_stats_json);
  installHttpHandler("vars", &Master::http_vars);
}


void Master::newMasterDetected(const UPID& pid)
{
  // Check and see if we are (1) still waiting to be the active
  // master, (2) newly active master, (3) no longer active master,
  // or (4) still active master.

  UPID master = pid;

  if (master != self() && !active) {
    LOG(INFO) << "Waiting to be master!";
  } else if (master == self() && !active) {
    LOG(INFO) << "Acting as master!";
    active = true;
  } else if (master != self() && active) {
    LOG(FATAL) << "No longer active master ... committing suicide!";
  } else if (master == self() && active) {
    LOG(INFO) << "Still acting as master!";
  }
}


void Master::noMasterDetected()
{
  if (active) {
    LOG(FATAL) << "No longer active master ... committing suicide!";
  } else {
    LOG(FATAL) << "No master detected (?) ... committing suicide!";
  }
}


void Master::registerFramework(const FrameworkInfo& frameworkInfo)
{
  Framework* framework =
    new Framework(frameworkInfo, newFrameworkId(), from(), elapsedTime());

  LOG(INFO) << "Registering " << framework << " at " << from();

  if (framework->info.executor().uri() == "") {
    LOG(INFO) << framework << " registering without an executor URI";
    FrameworkErrorMessage message;
    message.set_code(1);
    message.set_message("No executor URI given");
    send(from(), message);
    delete framework;
  } else {
    bool rootSubmissions = conf.get<bool>("root_submissions", true);
    if (framework->info.user() == "root" && rootSubmissions == false) {
      LOG(INFO) << framework << " registering as root, but "
                << "root submissions are disabled on this cluster";
      FrameworkErrorMessage message;
      message.set_code(1);
      message.set_message("User 'root' is not allowed to run frameworks");
      send(from(), message);
      delete framework;
    }
  }

  addFramework(framework);
}


void Master::reregisterFramework(const FrameworkID& frameworkId,
                                 const FrameworkInfo& frameworkInfo,
                                 int32_t generation)
{
  if (frameworkId == "") {
    LOG(ERROR) << "Framework re-registering without an id!";
    FrameworkErrorMessage message;
    message.set_code(1);
    message.set_message("Missing framework id");
    send(from(), message);
  } else if (frameworkInfo.executor().uri() == "") {
    LOG(INFO) << "Framework " << frameworkId << " re-registering "
              << "without an executor URI";
    FrameworkErrorMessage message;
    message.set_code(1);
    message.set_message("No executor URI given");
    send(from(), message);
  } else {
    LOG(INFO) << "Re-registering framework " << frameworkId
              << " at " << from();

    if (frameworks.count(frameworkId) > 0) {
      // Using the "generation" of the scheduler allows us to keep a
      // scheduler that got partitioned but didn't die (in ZooKeeper
      // speak this means didn't lose their session) and then
      // eventually tried to connect to this master even though
      // another instance of their scheduler has reconnected. This
      // might not be an issue in the future when the
      // master/allocator launches the scheduler can get restarted
      // (if necessary) by the master and the master will always
      // know which scheduler is the correct one.
      if (generation == 0) {
        // TODO: Should we check whether the new scheduler has given
        // us a different framework name, user name or executor info?
        LOG(INFO) << "Framework " << frameworkId << " failed over";
        failoverFramework(frameworks[frameworkId], from());
      } else {
        LOG(INFO) << "Framework " << frameworkId
                  << " re-registering with an already used id "
                  << " and not failing over!";
        FrameworkErrorMessage message;
        message.set_code(1);
        message.set_message("Framework id in use");
        send(from(), message);
        return;
      }
    } else {
      // We don't have a framework with this ID, so we must be a newly
      // elected Mesos master to which either an existing scheduler or a
      // failed-over one is connecting. Create a Framework object and add
      // any tasks it has that have been reported by reconnecting slaves.
      Framework* framework =
        new Framework(frameworkInfo, frameworkId, from(), elapsedTime());

      // TODO(benh): Check for root submissions like above!

      addFramework(framework);
      // Add any running tasks reported by slaves for this framework.
      foreachpair (const SlaveID& slaveId, Slave* slave, slaves) {
        foreachvalue (Task* task, slave->tasks) {
          if (framework->id == task->framework_id()) {
            framework->addTask(task);
          }
        }
      }
    }

    CHECK(frameworks.count(frameworkId) > 0);

    // Broadcast the new framework pid to all the slaves. We have to
    // broadcast because an executor might be running on a slave but
    // it currently isn't running any tasks. This could be a
    // potential scalability issue ...
    foreachvalue (Slave* slave, slaves) {
      UpdateFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.set_pid(from());
      send(slave->pid, message);
    }
  }
}


void Master::unregisterFramework(const FrameworkID& frameworkId)
{
  LOG(INFO) << "Asked to unregister framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    if (framework->pid == from()) {
      removeFramework(framework);
    } else {
      LOG(WARNING) << from() << " tried to unregister framework; "
                   << "expecting " << framework->pid;
    }
  }
}


void Master::resourceOfferReply(const FrameworkID& frameworkId,
                                const OfferID& offerId,
                                const vector<TaskDescription>& tasks,
                                const Params& params)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Offer* offer = getOffer(offerId);
    if (offer != NULL) {
      processOfferReply(offer, tasks, params);
    } else {
      // The slot offer is gone, meaning that we rescinded it, it
      // has already been replied to, or that the slave was lost;
      // immediately report any tasks in it as lost (it would
      // probably be better to have better error messages here).
      foreach (const TaskDescription& task, tasks) {
        StatusUpdateMessage message;
        StatusUpdate* update = message.mutable_update();
        update->mutable_framework_id()->MergeFrom(frameworkId);
        update->mutable_executor_id()->MergeFrom(task.executor().executor_id());
        update->mutable_slave_id()->MergeFrom(task.slave_id());
        TaskStatus* status = update->mutable_status();
        status->mutable_task_id()->MergeFrom(task.task_id());
        status->set_state(TASK_LOST);
        update->set_timestamp(elapsedTime());
	update->set_uuid(UUID::random().toBytes());
        send(framework->pid, message);
      }
    }
  }
}


void Master::reviveOffers(const FrameworkID& frameworkId)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Reviving offers for " << framework;
    framework->slaveFilter.clear();
    allocator->offersRevived(framework);
  }
}


void Master::killTask(const FrameworkID& frameworkId,
                      const TaskID& taskId)
{
  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Task* task = framework->getTask(taskId);
    if (task != NULL) {
      Slave* slave = getSlave(task->slave_id());
      CHECK(slave != NULL);

      LOG(INFO) << "Telling slave " << slave->id
                << " to kill task " << taskId
                << " of framework " << frameworkId;

      KillTaskMessage message;
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_task_id()->MergeFrom(taskId);
      send(slave->pid, message);
    } else {
      // TODO(benh): Once the scheduler has persistance and
      // high-availability of it's tasks, it will be the one that
      // determines that this invocation of 'killTask' is silly, and
      // can just return "locally" (i.e., after hitting only the other
      // replicas). Unfortunately, it still won't know the slave id.

      LOG(WARNING) << "Cannot kill task " << taskId
                   << " of framework " << frameworkId
                   << " because it cannot be found";
      StatusUpdateMessage message;
      StatusUpdate* update = message.mutable_update();
      update->mutable_framework_id()->MergeFrom(frameworkId);
      TaskStatus* status = update->mutable_status();
      status->mutable_task_id()->MergeFrom(taskId);
      status->set_state(TASK_LOST);
      update->set_timestamp(elapsedTime());
      update->set_uuid(UUID::random().toBytes());
      send(framework->pid, message);
    }
  }
}


void Master::schedulerMessage(const SlaveID& slaveId,
			      const FrameworkID& frameworkId,
			      const ExecutorID& executorId,
                              const string& data)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Slave* slave = getSlave(slaveId);
    if (slave != NULL) {
      LOG(INFO) << "Sending framework message for framework "
                << frameworkId << " to slave " << slaveId;

      FrameworkToExecutorMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      send(slave->pid, message);

      stats.validFrameworkMessages++;
    } else {
      LOG(WARNING) << "Cannot send framework message for framework "
                   << frameworkId << " to slave " << slaveId
                   << " because slave does not exist";
      stats.invalidFrameworkMessages++;
    }
  } else {
    LOG(WARNING) << "Cannot send framework message for framework "
                 << frameworkId << " to slave " << slaveId
                 << " because framework does not exist";
    stats.invalidFrameworkMessages++;
  }
}


void Master::registerSlave(const SlaveInfo& slaveInfo)
{
  Slave* slave = new Slave(slaveInfo, newSlaveId(), from(), elapsedTime());

  LOG(INFO) << "Attempting to register slave " << slave->id
            << " at " << slave->pid;

  // Checks if this slave, or if all slaves, can be accepted.
  if (slaveHostnamePorts.count(slaveInfo.hostname(), from().port) > 0) {
    run(&SlaveRegistrar::run, slave, self());
  } else if (conf.get<string>("slaves", "*") == "*") {
    run(&SlaveRegistrar::run, slave, self(), slavesManager->self());
  } else {
    LOG(WARNING) << "Cannot register slave at "
                 << slaveInfo.hostname() << ":" << from().port
                 << " because not in allocated set of slaves!";
    send(from(), TERMINATE);
  }
}


void Master::reregisterSlave(const SlaveID& slaveId,
                             const SlaveInfo& slaveInfo,
                             const vector<Task>& tasks)
{
  if (slaveId == "") {
    LOG(ERROR) << "Slave re-registered without an id!";
    send(from(), TERMINATE);
  } else {
    Slave* slave = getSlave(slaveId);
    if (slave != NULL) {
      // TODO(benh): It's still unclear whether or not
      // MasterDetector::detectMaster will cause spurious
      // Slave::newMasterDetected to get invoked even though the
      // ephemeral znode hasn't changed. If that does happen, the
      // re-register that the slave is trying to do is just
      // bogus. Letting it re-register might not be all that bad now,
      // but maybe in the future it's bad because during that
      // "disconnected" time it might not have received certain
      // messages from us (like launching a task), and so until we
      // have some form of task reconciliation between all the
      // different components, the safe thing to do is have the slave
      // restart (kind of defeats the purpose of session expiration
      // support in ZooKeeper if the spurious calls happen each time).
      LOG(ERROR) << "Slave at " << from()
		 << " attempted to re-register with an already in use id ("
		 << slaveId << ")";
      send(from(), TERMINATE);
    } else {
      Slave* slave = new Slave(slaveInfo, slaveId, from(), elapsedTime());

      LOG(INFO) << "Attempting to re-register slave " << slave->id
                << " at " << slave->pid;

      // Checks if this slave, or if all slaves, can be accepted.
      if (slaveHostnamePorts.count(slaveInfo.hostname(), from().port) > 0) {
        run(&SlaveReregistrar::run, slave, tasks, self());
      } else if (conf.get<string>("slaves", "*") == "*") {
        run(&SlaveReregistrar::run,
            slave, tasks, self(), slavesManager->self());
      } else {
        LOG(WARNING) << "Cannot re-register slave at "
                     << slaveInfo.hostname() << ":" << from().port
                     << " because not in allocated set of slaves!";
        send(from(), TERMINATE);
      }
    }
  }
}


void Master::unregisterSlave(const SlaveID& slaveId)
{
  LOG(INFO) << "Asked to unregister slave " << slaveId;

  // TODO(benh): Check that only the slave is asking to unregister?

  Slave* slave = getSlave(slaveId);
  if (slave != NULL) {
    removeSlave(slave);
  }
}


void Master::statusUpdate(const StatusUpdate& update, const UPID& pid)
{
  const TaskStatus& status = update.status();

  LOG(INFO) << "Status update from " << from()
            << ": task " << status.task_id()
            << " of framework " << update.framework_id()
            << " is now in state " << status.state();

  Slave* slave = getSlave(update.slave_id());
  if (slave != NULL) {
    Framework* framework = getFramework(update.framework_id());
    if (framework != NULL) {
      // Pass on the (transformed) status update to the framework.
      StatusUpdateMessage message;
      message.mutable_update()->MergeFrom(update);
      message.set_pid(pid);
      send(framework->pid, message);

      // Lookup the task and see if we need to update anything locally.
      Task* task = slave->getTask(update.framework_id(), status.task_id());
      if (task != NULL) {
        task->set_state(status.state());

        // Handle the task appropriately if it's terminated.
        if (status.state() == TASK_FINISHED ||
            status.state() == TASK_FAILED ||
            status.state() == TASK_KILLED ||
            status.state() == TASK_LOST) {
          removeTask(framework, slave, task, TRR_TASK_ENDED);
        }

        stats.tasks[status.state()]++;

        stats.validStatusUpdates++;
      } else {
        LOG(WARNING) << "Status update from " << from()
                     << ": error, couldn't lookup "
                     << "task " << status.task_id();
	stats.invalidStatusUpdates++;
      }
    } else {
      LOG(WARNING) << "Status update from " << from()
                   << ": error, couldn't lookup "
                   << "framework " << update.framework_id();
      stats.invalidStatusUpdates++;
    }
  } else {
    LOG(WARNING) << "Status update from " << from()
                 << ": error, couldn't lookup slave "
                 << update.slave_id();
    stats.invalidStatusUpdates++;
  }
}


void Master::executorMessage(const SlaveID& slaveId,
			     const FrameworkID& frameworkId,
			     const ExecutorID& executorId,
                             const string& data)
{
  Slave* slave = getSlave(slaveId);
  if (slave != NULL) {
    Framework* framework = getFramework(frameworkId);
    if (framework != NULL) {
      LOG(INFO) << "Sending framework message from slave " << slaveId
                << " to framework " << frameworkId;
      ExecutorToFrameworkMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      send(framework->pid, message);

      stats.validFrameworkMessages++;
    } else {
      LOG(WARNING) << "Cannot send framework message from slave "
                   << slaveId << " to framework " << frameworkId
                   << " because framework does not exist";
      stats.invalidFrameworkMessages++;
    }
  } else {
    LOG(WARNING) << "Cannot send framework message from slave "
                 << slaveId << " to framework " << frameworkId
                 << " because slave does not exist";
    stats.invalidFrameworkMessages++;
  }
}


void Master::exitedExecutor(const SlaveID& slaveId,
                            const FrameworkID& frameworkId,
                            const ExecutorID& executorId,
                            int32_t status)
{
  // TODO(benh): Send status updates for the tasks running under this
  // executor from the slave! Maybe requires adding an extra "reason"
  // so that people can see that the tasks were lost because of 

  Slave* slave = getSlave(slaveId);
  if (slave != NULL) {
    Framework* framework = getFramework(frameworkId);
    if (framework != NULL) {
      LOG(INFO) << "Executor " << executorId
                << " of framework " << framework->id
                << " on slave " << slave->id
                << " (" << slave->info.hostname() << ") "
                << "exited with status " << status;

      // TODO(benh): What about a status update that is on it's way
      // from the slave but got re-ordered on the wire? By sending
      // this status updates here we are not allowing possibly
      // finished tasks to reach the scheduler appropriately. In
      // stead, it seems like perhaps the right thing to do is to have
      // the slave be responsible for sending those status updates,
      // and have the master (or better yet ... and soon ... the
      // scheduler) decide that a task is dead only when a slave lost
      // has occured.

      // Tell the framework which tasks have been lost.
      foreachvalue (Task* task, utils::copy(framework->tasks)) {
        if (task->slave_id() == slave->id &&
            task->executor_id() == executorId) {
          StatusUpdateMessage message;
          StatusUpdate* update = message.mutable_update();
          update->mutable_framework_id()->MergeFrom(task->framework_id());
          update->mutable_executor_id()->MergeFrom(task->executor_id());
          update->mutable_slave_id()->MergeFrom(task->slave_id());
          TaskStatus* status = update->mutable_status();
          status->mutable_task_id()->MergeFrom(task->task_id());
          status->set_state(TASK_LOST);
          update->set_timestamp(elapsedTime());
	  update->set_uuid(UUID::random().toBytes());
          send(framework->pid, message);

          LOG(INFO) << "Removing task " << task->task_id()
                    << " of framework " << frameworkId
                    << " because of lost executor";

          removeTask(framework, slave, task, TRR_EXECUTOR_LOST);
        }
      }

      // TODO(benh): Send the framework it's executor's exit
      // status? Or maybe at least have something like
      // M2F_EXECUTOR_LOST?
    }
  }
}


void Master::activatedSlaveHostnamePort(const string& hostname, uint16_t port)
{
  LOG(INFO) << "Master now considering a slave at "
            << hostname << ":" << port << " as active";
  slaveHostnamePorts.insert(hostname, port);
}


void Master::deactivatedSlaveHostnamePort(const string& hostname,
                                          uint16_t port)
{
  if (slaveHostnamePorts.count(hostname, port) > 0) {
    // Look for a connected slave and remove it.
    foreachvalue (Slave* slave, slaves) {
      if (slave->info.hostname() == hostname && slave->pid.port == port) {
        LOG(WARNING) << "Removing slave " << slave->id << " at "
		     << hostname << ":" << port
                     << " because it has been deactivated";
	send(slave->pid, TERMINATE);
        removeSlave(slave);
        break;
      }
    }

    LOG(INFO) << "Master now considering a slave at "
	      << hostname << ":" << port << " as inactive";
    slaveHostnamePorts.erase(hostname, port);
  }
}


void Master::timerTick()
{
  // Check which framework filters can be expired.
  foreachvalue (Framework* framework, frameworks) {
    framework->removeExpiredFilters(elapsedTime());
  }

  // Do allocations!
  allocator->timerTick();

  // Scheduler another timer tick!
  delay(1.0, self(), &Master::timerTick);
}


void Master::frameworkFailoverTimeout(const FrameworkID& frameworkId,
                                      double reregisteredTime)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL && !framework->active &&
      framework->reregisteredTime == reregisteredTime) {
    LOG(INFO) << "Framework failover timeout, removing framework "
              << framework->id;
    removeFramework(framework);
  }
}


void Master::exited()
{
  foreachvalue (Framework* framework, frameworks) {
    if (framework->pid == from()) {
      LOG(INFO) << "Framework " << framework->id << " disconnected";

//       removeFramework(framework);

      // Stop sending offers here for now.
      framework->active = false;

      // Delay dispatching a message to ourselves for the timeout.
      delay(FRAMEWORK_FAILOVER_TIMEOUT, self(),
            &Master::frameworkFailoverTimeout,
            framework->id, framework->reregisteredTime);

      // Remove the framework's slot offers.
      foreach (Offer* offer, utils::copy(framework->offers)) {
        removeOffer(offer, ORR_FRAMEWORK_FAILOVER, offer->resources);
      }
      return;
    }
  }

  foreachvalue (Slave* slave, slaves) {
    if (slave->pid == from()) {
      LOG(INFO) << "Slave " << slave->id << " disconnected";
      removeSlave(slave);
      return;
    }
  }
}


OfferID Master::makeOffer(Framework* framework,
                          const vector<SlaveResources>& resources)
{
  const OfferID& offerId = newOfferId();

  Offer* offer = new Offer(offerId, framework->id, resources);

  offers[offer->id] = offer;
  framework->addOffer(offer);

  // Update the resource information within each of the slave objects. Gross!
  foreach (const SlaveResources& r, resources) {
    r.slave->offers.insert(offer);
    r.slave->resourcesOffered += r.resources;
  }

  LOG(INFO) << "Sending offer " << offer->id
            << " to framework " << framework->id;

  ResourceOfferMessage message;
  message.mutable_offer_id()->MergeFrom(offerId);

  foreach (const SlaveResources& r, resources) {
    SlaveOffer* offer = message.add_offers();
    offer->mutable_slave_id()->MergeFrom(r.slave->id);
    offer->set_hostname(r.slave->info.hostname());
    offer->mutable_resources()->MergeFrom(r.resources);

    message.add_pids(r.slave->pid);
  }

  send(framework->pid, message);

  return offerId;
}


// Process a resource offer reply (for a non-cancelled offer) by launching
// the desired tasks (if the offer contains a valid set of tasks) and
// reporting any unused resources to the allocator.
void Master::processOfferReply(Offer* offer,
                               const vector<TaskDescription>& tasks,
                               const Params& params)
{
  LOG(INFO) << "Received reply for " << offer;

  Framework* framework = getFramework(offer->frameworkId);
  CHECK(framework != NULL);

  // Count resources in the offer.
  hashmap<Slave*, Resources> resourcesOffered;
  foreach (const SlaveResources& r, offer->resources) {
    resourcesOffered[r.slave] = r.resources;
  }

  // Count used resources and check that its tasks are valid.
  hashmap<Slave*, Resources> resourcesUsed;
  foreach (const TaskDescription& task, tasks) {
    // Check whether the task is on a valid slave.
    Slave* slave = getSlave(task.slave_id());
    if (slave == NULL || resourcesOffered.count(slave) == 0) {
      terminateFramework(framework, 0, "Invalid slave in offer reply");
      return;
    }

    // Check whether or not the resources for the task are valid.
    // TODO(benh): In the future maybe we can also augment the
    // protobuf to deal with fragmentation purposes by providing some
    // sort of minimum amount of resources required per task.

    if (task.resources().size() == 0) {
      terminateFramework(framework, 0, "Invalid resources for task");
      return;
    }

    foreach (const Resource& resource, task.resources()) {
      if (!Resources::isAllocatable(resource)) {
        // TODO(benh): Also send back the invalid resources as a string?
        terminateFramework(framework, 0, "Invalid resources for task");
        return;
      }
    }

    resourcesUsed[slave] += task.resources();
  }

  // Check that the total accepted on each slave isn't more than offered.
  foreachpair (Slave* slave, const Resources& used, resourcesUsed) {
    if (!(used <= resourcesOffered[slave])) {
      terminateFramework(framework, 0, "Too many resources accepted");
      return;
    }
  }

  // Check that there are no duplicate task IDs.
  hashset<TaskID> idsInResponse;
  foreach (const TaskDescription& task, tasks) {
    if (framework->tasks.count(task.task_id()) > 0 ||
        idsInResponse.count(task.task_id()) > 0) {
      string error = "Duplicate task ID: " + task.task_id().value();
      terminateFramework(framework, 0, error);
      return;
    }
    idsInResponse.insert(task.task_id());
  }

  // Launch the tasks in the response.
  foreach (const TaskDescription& task, tasks) {
    launchTask(framework, task);
  }

  // Get out the timeout for left over resources (if exists), and use
  // that to calculate the expiry timeout.
  double timeout = DEFAULT_REFUSAL_TIMEOUT;

  for (int i = 0; i < params.param_size(); i++) {
    if (params.param(i).key() == "timeout") {
      try {
        timeout = boost::lexical_cast<double>(params.param(i).value());
      } catch (boost::bad_lexical_cast&) {
        string error = "Failed to convert value '" +
          params.param(i).value() + "' for key 'timeout' to an integer";
        terminateFramework(framework, 0, error);
        return;
      }
      break;
    }
  }

  double expiry = (timeout == -1) ? 0 : elapsedTime() + timeout;  

  // Now check for unused resources on slaves and add filters for them.
  vector<SlaveResources> resourcesUnused;

  foreachpair (Slave* slave, const Resources& offered, resourcesOffered) {
    Resources used = resourcesUsed[slave];
    Resources unused = offered - used;

    CHECK(used == used.allocatable());

    Resources allocatable = unused.allocatable();

    if (allocatable.size() > 0) {
      resourcesUnused.push_back(SlaveResources(slave, allocatable));
    }

    // Only add a filter on a slave if none of the resources are used.
    if (timeout != 0 && used.size() == 0) {
      LOG(INFO) << "Adding filter on " << slave << " to " << framework
                << " for " << timeout << " seconds";
      framework->slaveFilter[slave] = expiry;
    }
  }
  
  // Return the resources left to the allocator.
  removeOffer(offer, ORR_FRAMEWORK_REPLIED, resourcesUnused);
}


void Master::launchTask(Framework* framework, const TaskDescription& task)
{
  // The invariant right now is that launchTask is called only for
  // TaskDescriptions where the slave is still valid (see the code
  // above in processOfferReply).
  Slave* slave = getSlave(task.slave_id());
  CHECK(slave != NULL);

  // Determine the executor ID for this task.
  const ExecutorID& executorId = task.has_executor()
    ? task.executor().executor_id()
    : framework->info.executor().executor_id();

  Task* t = new Task();
  t->mutable_framework_id()->MergeFrom(framework->id);
  t->mutable_executor_id()->MergeFrom(executorId);
  t->set_state(TASK_STARTING);
  t->set_name(task.name());
  t->mutable_task_id()->MergeFrom(task.task_id());
  t->mutable_slave_id()->MergeFrom(task.slave_id());
  t->mutable_resources()->MergeFrom(task.resources());

  framework->addTask(t);
  slave->addTask(t);

  allocator->taskAdded(t);

  LOG(INFO) << "Launching " << t << " on " << slave;

  RunTaskMessage message;
  message.mutable_framework()->MergeFrom(framework->info);
  message.mutable_framework_id()->MergeFrom(framework->id);
  message.set_pid(framework->pid);
  message.mutable_task()->MergeFrom(task);
  send(slave->pid, message);

  stats.tasks[TASK_STARTING]++;
}


void Master::addFramework(Framework* framework)
{
  CHECK(frameworks.count(framework->id) == 0);

  frameworks[framework->id] = framework;

  link(framework->pid);

  FrameworkRegisteredMessage message;
  message.mutable_framework_id()->MergeFrom(framework->id);
  send(framework->pid, message);

  allocator->frameworkAdded(framework);
}


// Replace the scheduler for a framework with a new process ID, in the
// event of a scheduler failover.
void Master::failoverFramework(Framework* framework, const UPID& newPid)
{
  const UPID& oldPid = framework->pid;

  // Remove the framework's slot offers (if they weren't removed before).
  // TODO(benh): Consider just reoffering these to the new framework.
  foreach (Offer* offer, utils::copy(framework->offers)) {
    removeOffer(offer, ORR_FRAMEWORK_FAILOVER, offer->resources);
  }

  {
    FrameworkErrorMessage message;
    message.set_code(1);
    message.set_message("Framework failover");
    send(oldPid, message);
  }

  // TODO(benh): unlink(oldPid);

  framework->pid = newPid;
  link(newPid);

  // Make sure we can get offers again.
  framework->active = true;

  framework->reregisteredTime = elapsedTime();

  FrameworkRegisteredMessage message;
  message.mutable_framework_id()->MergeFrom(framework->id);
  send(newPid, message);
}


void Master::terminateFramework(Framework* framework,
                                int32_t code,
                                const string& error)
{
  LOG(INFO) << "Terminating " << framework << " due to error: " << error;

  FrameworkErrorMessage message;
  message.set_code(code);
  message.set_message(error);
  send(framework->pid, message);

  removeFramework(framework);
}


void Master::removeFramework(Framework* framework)
{
  framework->active = false;
  // TODO: Notify allocator that a framework removal is beginning?
  
  // Tell slaves to shutdown the framework.
  foreachvalue (Slave* slave, slaves) {
    ShutdownFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id);
    send(slave->pid, message);
  }

  // Remove pointers to the framework's tasks in slaves
  foreachvalue (Task* task, utils::copy(framework->tasks)) {
    Slave* slave = getSlave(task->slave_id());
    CHECK(slave != NULL);
    removeTask(framework, slave, task, TRR_FRAMEWORK_LOST);
  }
  
  // Remove the framework's slot offers (if they weren't removed before).
  foreach (Offer* offer, utils::copy(framework->offers)) {
    removeOffer(offer, ORR_FRAMEWORK_LOST, offer->resources);
  }

  // TODO(benh): Similar code between removeFramework and
  // failoverFramework needs to be shared!

  // TODO(benh): unlink(framework->pid);

  // Delete it.
  frameworks.erase(framework->id);
  allocator->frameworkRemoved(framework);
  delete framework;
}


void Master::addSlave(Slave* slave, bool reregister)
{
  CHECK(slave != NULL);

  slaves[slave->id] = slave;

  link(slave->pid);

  allocator->slaveAdded(slave);

  if (!reregister) {
    SlaveRegisteredMessage message;
    message.mutable_slave_id()->MergeFrom(slave->id);
    send(slave->pid, message);
  } else {
    SlaveReregisteredMessage message;
    message.mutable_slave_id()->MergeFrom(slave->id);
    send(slave->pid, message);
  }

  // TODO(benh):
  //     // Ask the slaves manager to monitor this slave for us.
  //     dispatch(slavesManager->self(), &SlavesManager::monitor,
  //              slave->pid, slave->info, slave->id);

  // Set up an observer for the slave.
  slave->observer = new SlaveObserver(slave->pid, slave->info,
                                      slave->id, slavesManager->self());
  spawn(slave->observer);
}


void Master::readdSlave(Slave* slave, const vector<Task>& tasks)
{
  CHECK(slave != NULL);

  addSlave(slave, true);

  for (int i = 0; i < tasks.size(); i++) {
    Task* task = new Task(tasks[i]);

    // Add the task to the slave.
    slave->addTask(task);

    // Try and add the task to the framework too, but since the
    // framework might not yet be connected we won't be able to
    // add them. However, when the framework connects later we
    // will add them then. We also tell this slave the current
    // framework pid for this task. Again, we do the same thing
    // if a framework currently isn't registered.
    Framework* framework = getFramework(task->framework_id());
    if (framework != NULL) {
      framework->addTask(task);
      UpdateFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.set_pid(framework->pid);
      send(slave->pid, message);
    } else {
      // TODO(benh): We should really put a timeout on how long we
      // keep tasks running on a slave that never have frameworks
      // reregister and claim them.
      LOG(WARNING) << "Possibly orphaned task " << task->task_id()
                   << " of framework " << task->framework_id()
                   << " running on slave " << slave->id;
    }
  }
}


// Lose all of a slave's tasks and delete the slave object
void Master::removeSlave(Slave* slave)
{ 
  slave->active = false;

  // TODO: Notify allocator that a slave removal is beginning?
  
  // Remove pointers to slave's tasks in frameworks, and send status updates
  foreachvalue (Task* task, utils::copy(slave->tasks)) {
    Framework* framework = getFramework(task->framework_id());
    // A framework might not actually exist because the master failed
    // over and the framework hasn't reconnected. This can be a tricky
    // situation for frameworks that want to have high-availability,
    // because if they eventually do connect they won't ever get a
    // status update about this task.  Perhaps in the future what we
    // want to do is create a local Framework object to represent that
    // framework until it fails over. See the TODO above in
    // Master::reregisterSlave.
    if (framework != NULL) {
      StatusUpdateMessage message;
      StatusUpdate* update = message.mutable_update();
      update->mutable_framework_id()->MergeFrom(task->framework_id());
      update->mutable_executor_id()->MergeFrom(task->executor_id());
      update->mutable_slave_id()->MergeFrom(task->slave_id());
      TaskStatus* status = update->mutable_status();
      status->mutable_task_id()->MergeFrom(task->task_id());
      status->set_state(TASK_LOST);
      update->set_timestamp(elapsedTime());
      update->set_uuid(UUID::random().toBytes());
      send(framework->pid, message);
    }
    removeTask(framework, slave, task, TRR_SLAVE_LOST);
  }

  // Remove slot offers from the slave; this will also rescind them
  foreach (Offer* offer, utils::copy(slave->offers)) {
    // Only report resources on slaves other than this one to the allocator
    vector<SlaveResources> otherSlaveResources;
    foreach (const SlaveResources& r, offer->resources) {
      if (r.slave != slave) {
        otherSlaveResources.push_back(r);
      }
    }
    removeOffer(offer, ORR_SLAVE_LOST, otherSlaveResources);
  }
  
  // Remove slave from any filters
  foreachvalue (Framework* framework, frameworks) {
    framework->slaveFilter.erase(slave);
  }
  
  // Send lost-slave message to all frameworks (this helps them re-run
  // previously finished tasks whose output was on the lost slave)
  foreachvalue (Framework* framework, frameworks) {
    LostSlaveMessage message;
    message.mutable_slave_id()->MergeFrom(slave->id);
    send(framework->pid, message);
  }

  // TODO(benh):
  //     // Tell the slaves manager to stop monitoring this slave for us.
  //     dispatch(slavesManager->self(), &SlavesManager::forget,
  //              slave->pid, slave->info, slave->id);

  // Kill the slave observer.
  terminate(slave->observer);
  wait(slave->observer);

  delete slave->observer;

  // TODO(benh): unlink(slave->pid);

  // Delete it
  slaves.erase(slave->id);
  allocator->slaveRemoved(slave);
  delete slave;
}


// Remove a slot offer (because it was replied or we lost a framework or slave)
void Master::removeTask(Framework* framework,
                        Slave* slave,
                        Task* task,
                        TaskRemovalReason reason)
{
  framework->removeTask(task->task_id());
  slave->removeTask(task);
  allocator->taskRemoved(task, reason);
  delete task;
}


void Master::removeOffer(Offer* offer,
                         OfferReturnReason reason,
                         const vector<SlaveResources>& resourcesUnused)
{
  // Remove from slaves.
  foreach (SlaveResources& r, offer->resources) {
    CHECK(r.slave != NULL);
    r.slave->resourcesOffered -= r.resources;
    r.slave->offers.erase(offer);
  }
    
  // Remove from framework
  Framework *framework = getFramework(offer->frameworkId);
  CHECK(framework != NULL);
  framework->removeOffer(offer);

  // Also send framework a rescind message unless the reason we are
  // removing the offer is that the framework replied to it
  if (reason != ORR_FRAMEWORK_REPLIED) {
    RescindResourceOfferMessage message;
    message.mutable_offer_id()->MergeFrom(offer->id);
    send(framework->pid, message);
  }
  
  // Tell the allocator about the unused resources.
  allocator->offerReturned(offer, reason, resourcesUnused);
  
  // Delete it
  offers.erase(offer->id);
  delete offer;
}


Framework* Master::getFramework(const FrameworkID& frameworkId)
{
  if (frameworks.count(frameworkId) > 0) {
    return frameworks[frameworkId];
  } else {
    return NULL;
  }
}


Slave* Master::getSlave(const SlaveID& slaveId)
{
  if (slaves.count(slaveId) > 0) {
    return slaves[slaveId];
  } else {
    return NULL;
  }
}


Offer* Master::getOffer(const OfferID& offerId)
{
  if (offers.count(offerId) > 0) {
    return offers[offerId];
  } else {
    return NULL;
  }
}


// Create a new framework ID. We format the ID as MASTERID-FWID, where
// MASTERID is the ID of the master (launch date plus fault tolerant ID)
// and FWID is an increasing integer.
FrameworkID Master::newFrameworkId()
{
  std::ostringstream out;

  out << masterId << "-" << std::setw(4)
      << std::setfill('0') << nextFrameworkId++;

  FrameworkID frameworkId;
  frameworkId.set_value(out.str());

  return frameworkId;
}


OfferID Master::newOfferId()
{
  OfferID offerId;
  offerId.set_value(masterId + "-" + utils::stringify(nextOfferId++));
  return offerId;
}


SlaveID Master::newSlaveId()
{
  SlaveID slaveId;
  slaveId.set_value(masterId + "-" + utils::stringify(nextSlaveId++));
  return slaveId;
}


Promise<HttpResponse> Master::http_info_json(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/master/info.json'";

  std::ostringstream out;

  out <<
    "{" <<
    "\"built_date\":\"" << build::DATE << "\"," <<
    "\"build_user\":\"" << build::USER << "\"," <<
    "\"start_time\":\"" << startTime << "\"," <<
    "\"pid\":\"" << self() << "\"" <<
    "}";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Master::http_frameworks_json(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/master/frameworks.json'";

  std::ostringstream out;

  out << "[";

  foreachvalue (Framework* framework, frameworks) {
    out <<
      "{" <<
      "\"id\":\"" << framework->id << "\"," <<
      "\"name\":\"" << framework->info.name() << "\"," <<
      "\"user\":\"" << framework->info.user() << "\""
      "},";
  }

  // Backup the put pointer to overwrite the last comma (hack).
  if (frameworks.size() > 0) {
    long pos = out.tellp();
    out.seekp(pos - 1);
  }

  out << "]";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Master::http_slaves_json(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/master/slaves.json'";

  std::ostringstream out;

  out << "[";

  foreachvalue (Slave* slave, slaves) {
    // TODO(benh): Send all of the resources (as JSON).
    Resources resources(slave->info.resources());
    Resource::Scalar cpus = resources.getScalar("cpus", Resource::Scalar());
    Resource::Scalar mem = resources.getScalar("mem", Resource::Scalar());
    out <<
      "{" <<
      "\"id\":\"" << slave->id << "\"," <<
      "\"hostname\":\"" << slave->info.hostname() << "\"," <<
      "\"cpus\":" << cpus.value() << "," <<
      "\"mem\":" << mem.value() <<
      "},";
  }

  // Backup the put pointer to overwrite the last comma (hack).
  if (slaves.size() > 0) {
    long pos = out.tellp();
    out.seekp(pos - 1);
  }

  out << "]";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Master::http_tasks_json(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/master/tasks.json'";

  std::ostringstream out;

  out << "[";

  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Task* task, framework->tasks) {
      // TODO(benh): Send all of the resources (as JSON).
      Resources resources(task->resources());
      Resource::Scalar cpus = resources.getScalar("cpus", Resource::Scalar());
      Resource::Scalar mem = resources.getScalar("mem", Resource::Scalar());
      out <<
        "{" <<
        "\"task_id\":\"" << task->task_id() << "\"," <<
        "\"framework_id\":\"" << task->framework_id() << "\"," <<
        "\"slave_id\":\"" << task->slave_id() << "\"," <<
        "\"name\":\"" << task->name() << "\"," <<
        "\"state\":\"" << task->state() << "\"," <<
        "\"cpus\":" << cpus.value() << "," <<
        "\"mem\":" << mem.value() <<
        "},";
    }
  }

  // Backup the put pointer to overwrite the last comma (hack).
  if (frameworks.size() > 0) {
    long pos = out.tellp();
    out.seekp(pos - 1);
  }

  out << "]";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Master::http_stats_json(const HttpRequest& request)
{
  LOG(INFO) << "Http request for '/master/stats.json'";

  std::ostringstream out;

  out << std::setprecision(10);

  out <<
    "{" <<
    "\"uptime\":" << elapsedTime() - startTime << "," <<
    "\"total_schedulers\":" << frameworks.size() << "," <<
    "\"active_schedulers\":" << getActiveFrameworks().size() << "," <<
    "\"activated_slaves\":" << slaveHostnamePorts.size() << "," <<
    "\"connected_slaves\":" << slaves.size() << "," <<
    "\"started_tasks\":" << stats.tasks[TASK_STARTING] << "," <<
    "\"finished_tasks\":" << stats.tasks[TASK_FINISHED] << "," <<
    "\"killed_tasks\":" << stats.tasks[TASK_KILLED] << "," <<
    "\"failed_tasks\":" << stats.tasks[TASK_FAILED] << "," <<
    "\"lost_tasks\":" << stats.tasks[TASK_LOST] << "," <<
    "\"valid_status_updates\":" << stats.validStatusUpdates << "," <<
    "\"invalid_status_updates\":" << stats.invalidStatusUpdates << "," <<
    "\"valid_framework_messages\":" << stats.validFrameworkMessages << "," <<
    "\"invalid_framework_messages\":" << stats.invalidFrameworkMessages <<
    "}";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Master::http_vars(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/master/vars'";

  std::ostringstream out;

  out <<
    "build_date " << build::DATE << "\n" <<
    "build_user " << build::USER << "\n" <<
    "build_flags " << build::FLAGS << "\n";

  // Also add the configuration values.
  foreachpair (const string& key, const string& value, conf.getMap()) {
    out << key << " " << value << "\n";
  }

  out << std::setprecision(10);

  out <<
    "uptime " << elapsedTime() - startTime << "\n" <<
    "total_schedulers " << frameworks.size() << "\n" <<
    "active_schedulers " << getActiveFrameworks().size() << "\n" <<
    "activated_slaves " << slaveHostnamePorts.size() << "\n" <<
    "connected_slaves " << slaves.size() << "\n" <<
    "started_tasks " << stats.tasks[TASK_STARTING] << "\n" <<
    "finished_tasks " << stats.tasks[TASK_FINISHED] << "\n" <<
    "killed_tasks " << stats.tasks[TASK_KILLED] << "\n" <<
    "failed_tasks " << stats.tasks[TASK_FAILED] << "\n" <<
    "lost_tasks " << stats.tasks[TASK_LOST] << "\n" <<
    "valid_status_updates " << stats.validStatusUpdates << "\n" <<
    "invalid_status_updates " << stats.invalidStatusUpdates << "\n" <<
    "valid_framework_messages " << stats.validFrameworkMessages << "\n" <<
    "invalid_framework_messages " << stats.invalidFrameworkMessages << "\n";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/plain";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}

}}} // namespace mesos { namespace master { namespace internal {
