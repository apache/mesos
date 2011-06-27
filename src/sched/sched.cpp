#include <dlfcn.h>
#include <errno.h>
#include <pwd.h>
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

#include "configurator/configuration.hpp"

#include "common/fatal.hpp"
#include "common/hashmap.hpp"
#include "common/lock.hpp"
#include "common/logging.hpp"
#include "common/type_utils.hpp"

#include "detector/detector.hpp"

#include "local/local.hpp"

#include "messages/messages.hpp"

using namespace mesos;
using namespace mesos::internal;

using namespace process;

using boost::cref;

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
      generation(0),
      master(UPID())
  {
    installProtobufHandler<NewMasterDetectedMessage>(
        &SchedulerProcess::newMasterDetected,
        &NewMasterDetectedMessage::pid);

    installProtobufHandler<NoMasterDetectedMessage>(
        &SchedulerProcess::noMasterDetected);

    installProtobufHandler<FrameworkRegisteredMessage>(
        &SchedulerProcess::registered,
        &FrameworkRegisteredMessage::framework_id);

    installProtobufHandler<ResourceOfferMessage>(
        &SchedulerProcess::resourceOffer,
        &ResourceOfferMessage::offer_id,
        &ResourceOfferMessage::offers,
        &ResourceOfferMessage::pids);

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

    installMessageHandler(process::EXITED, &SchedulerProcess::exited);
  }

  virtual ~SchedulerProcess() {}

protected:
  void newMasterDetected(const UPID& pid)
  {
    VLOG(1) << "New master at " << pid;

    master = pid;
    link(master);

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
      message.set_generation(generation++);
      send(master, message);
    }

    active = true;
  }

  void noMasterDetected()
  {
    VLOG(1) << "No master detected, waiting for another master";
    // In this case, we don't actually invoke Scheduler::error
    // since we might get reconnected to a master imminently.
    active = false;
  }

  void registered(const FrameworkID& frameworkId)
  {
    VLOG(1) << "Framework registered with " << frameworkId;
    this->frameworkId = frameworkId;
    invoke(bind(&Scheduler::registered, sched, driver, cref(frameworkId)));
  }

  void resourceOffer(const OfferID& offerId,
                     const vector<SlaveOffer>& offers,
                     const vector<string>& pids)
  {
    VLOG(1) << "Received offer " << offerId;

    // Save the pid associated with each slave (one per SlaveOffer) so
    // later we can send framework messages directly.
    CHECK(offers.size() == pids.size());

    for (int i = 0; i < offers.size(); i++) {
      UPID pid(pids[i]);
      if (pid != UPID()) {
	VLOG(2) << "Saving PID '" << pids[i] << "'";
	savedOffers[offerId][offers[i].slave_id()] = pid;
      } else {
	// Parsing of a PID may fail due to DNS! 
	VLOG(2) << "Failed to parse PID '" << pids[i] << "'";
      }
    }

    invoke(bind(&Scheduler::resourceOffer, sched, driver, cref(offerId),
                cref(offers)));
  }

  void rescindOffer(const OfferID& offerId)
  {
    VLOG(1) << "Rescinded offer " << offerId;
    savedOffers.erase(offerId);
    invoke(bind(&Scheduler::offerRescinded, sched, driver, cref(offerId)));
  }

  void statusUpdate(const StatusUpdate& update, const UPID& pid)
  {
    const TaskStatus& status = update.status();

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

    invoke(bind(&Scheduler::statusUpdate, sched, driver, cref(status)));

    if (pid) {
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
    VLOG(1) << "Lost slave " << slaveId;
    savedSlavePids.erase(slaveId);
    invoke(bind(&Scheduler::slaveLost, sched, driver, cref(slaveId)));
  }

  void frameworkMessage(const SlaveID& slaveId,
			const FrameworkID& frameworkId,
			const ExecutorID& executorId,
			const string& data)
  {
    VLOG(1) << "Received framework message";
    invoke(bind(&Scheduler::frameworkMessage, sched, driver, cref(slaveId),
                cref(executorId), cref(data)));
  }

  void error(int32_t code, const string& message)
  {
    VLOG(1) << "Got error '" << message << "' (code: " << code << ")";
    invoke(bind(&Scheduler::error, sched, driver, code, cref(message)));
  }

  void exited()
  {
    // TODO(benh): Don't wait for a new master forever.
    if (from() == master) {
      VLOG(1) << "Connection to master lost .. waiting for new master";
    }
  }

  void stop()
  {
    // Whether or not we send an unregister message, we want to
    // terminate this process ...
    terminate(self());

    if (!active)
      return;

    UnregisterFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    send(master, message);
  }

  void killTask(const TaskID& taskId)
  {
    if (!active)
      return;

    KillTaskMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_task_id()->MergeFrom(taskId);
    send(master, message);
  }

  void replyToOffer(const OfferID& offerId,
                    const vector<TaskDescription>& tasks,
                    const map<string, string>& params)
  {
    if (!active)
      return;

    ResourceOfferReplyMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_offer_id()->MergeFrom(offerId);

    foreachpair (const string& key, const string& value, params) {
      Param* param = message.mutable_params()->add_param();
      param->set_key(key);
      param->set_value(value);
    }

    foreach (const TaskDescription& task, tasks) {
      // Keep only the slave PIDs where we run tasks so we can send
      // framework messages directly.
      savedSlavePids[task.slave_id()] = savedOffers[offerId][task.slave_id()];

      message.add_tasks()->MergeFrom(task);
    }

    // Remove the offer since we saved all the PIDs we might use.
    savedOffers.erase(offerId);

    send(master, message);
  }

  void reviveOffers()
  {
    if (!active)
      return;

    ReviveOffersMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    send(master, message);
  }

  void sendFrameworkMessage(const SlaveID& slaveId,
			    const ExecutorID& executorId,
			    const string& data)
  {
    if (!active)
      return;

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
  int32_t generation;
  UPID master;

  volatile bool active;

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
// (2) There are two possible status variables, one called 'active' in
//     SchedulerProcess and one called 'running' in
//     MesosSchedulerDriver. The former is used to represent whether
//     or not we are connected to an active master while the latter is
//     used to represent whether or not a client has called
//     MesosSchedulerDriver::start/run or MesosSchedulerDriver::stop.


MesosSchedulerDriver::MesosSchedulerDriver(Scheduler* sched,
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
  init(sched, conf, frameworkId);
}


MesosSchedulerDriver::MesosSchedulerDriver(Scheduler* sched,
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
  init(sched, conf, frameworkId);
}


MesosSchedulerDriver::MesosSchedulerDriver(Scheduler* sched,
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
  init(sched, conf, frameworkId);
}


void MesosSchedulerDriver::init(Scheduler* _sched,
                                Configuration* _conf,
                                const FrameworkID& _frameworkId)
{
  sched = _sched;
  conf = _conf;
  frameworkId = _frameworkId;
  url = conf->get<string>("url", "local");
  process = NULL;
  detector = NULL;
  running = false;

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


int MesosSchedulerDriver::start()
{
  Lock lock(&mutex);

  if (running) {
    return -1;
  }

  // We might have been running before, but have since stopped. Don't
  // allow this driver to be used again (for now)!
  if (process != NULL) {
    return -1;
  }

  // Set running here so we can recognize an exception from calls into
  // Java (via getFrameworkName or getExecutorInfo).
  running = true;

  // Get username of current user.
  passwd* passwd;
  if ((passwd = getpwuid(getuid())) == NULL) {
    fatal("failed to get username information");
  }

  // Set up framework info.
  FrameworkInfo framework;
  framework.set_user(passwd->pw_name);
  framework.set_name(sched->getFrameworkName(this));
  framework.mutable_executor()->MergeFrom(sched->getExecutorInfo(this));

  // Something invoked stop while we were in the scheduler, bail.
  if (!running) {
    return -1;
  }

  process = new SchedulerProcess(this, sched, frameworkId, framework);

  UPID pid = spawn(process);

  // Check and see if we need to launch a local cluster.
  if (url == "local") {
    const PID<master::Master>& master = local::launch(*conf, true);
    detector = new BasicMasterDetector(master, pid);
  } else if (url == "localquiet") {
    conf->set("quiet", 1);
    const PID<master::Master>& master = local::launch(*conf, true);
    detector = new BasicMasterDetector(master, pid);
  } else {
    detector = MasterDetector::create(url, pid, false, false);
  }

  return 0;
}


int MesosSchedulerDriver::stop()
{
  Lock lock(&mutex);

  if (!running) {
    // Don't issue an error (could lead to an infinite loop).
    return -1;
  }

  // Stop the process if it is running (it might not be because we set
  // running to be true, then called getFrameworkName or
  // getExecutorInfo which threw exceptions, or explicitely called
  // stop. See above in start).
  if (process != NULL) {
    dispatch(process, &SchedulerProcess::stop);
  }

  running = false;

  // TODO: It might make more sense to clean up our local cluster here than in
  // the destructor. However, what would be even better is to allow multiple
  // local clusters to exist (i.e. not use global vars in local.cpp) so that
  // ours can just be an instance variable in MesosSchedulerDriver.

  pthread_cond_signal(&cond);

  return 0;
}


int MesosSchedulerDriver::join()
{
  Lock lock(&mutex);

  while (running) {
    pthread_cond_wait(&cond, &mutex);
  }

  return 0;
}


int MesosSchedulerDriver::run()
{
  int ret = start();
  return ret != 0 ? ret : join();
}


int MesosSchedulerDriver::killTask(const TaskID& taskId)
{
  Lock lock(&mutex);

  if (!running || (process != NULL && !process->active)) {
    return -1;
  }

  dispatch(process, &SchedulerProcess::killTask, taskId);

  return 0;
}


int MesosSchedulerDriver::replyToOffer(const OfferID& offerId,
                                       const vector<TaskDescription>& tasks,
                                       const map<string, string>& params)
{
  Lock lock(&mutex);

  if (!running || (process != NULL && !process->active)) {
    return -1;
  }

  dispatch(process, &SchedulerProcess::replyToOffer, offerId, tasks, params);

  return 0;
}


int MesosSchedulerDriver::reviveOffers()
{
  Lock lock(&mutex);

  if (!running || (process != NULL && !process->active)) {
    return -1;
  }

  dispatch(process, &SchedulerProcess::reviveOffers);

  return 0;
}


int MesosSchedulerDriver::sendFrameworkMessage(const SlaveID& slaveId,
					       const ExecutorID& executorId,
					       const string& data)
{
  Lock lock(&mutex);

  if (!running || (process != NULL && !process->active)) {
    return -1;
  }

  dispatch(process, &SchedulerProcess::sendFrameworkMessage,
           slaveId, executorId, data);

  return 0;
}


void MesosSchedulerDriver::error(int code, const string& message)
{
  sched->error(this, code, message);
}
