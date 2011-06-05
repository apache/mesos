#include <dlfcn.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <google/protobuf/descriptor.h>

#include <iostream>
#include <map>
#include <string>
#include <sstream>

#include <tr1/functional>

#include <mesos.hpp>
#include <mesos_sched.hpp>
#include <process.hpp>

#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>

#include "configurator/configuration.hpp"

#include "common/fatal.hpp"
#include "common/lock.hpp"
#include "common/logging.hpp"
#include "common/type_utils.hpp"

#include "detector/detector.hpp"

#include "local/local.hpp"

#include "messaging/messages.hpp"

using namespace mesos;
using namespace mesos::internal;

using boost::cref;
using boost::unordered_map;
using boost::unordered_set;

using google::protobuf::RepeatedPtrField;

using process::PID;
using process::UPID;

using std::map;
using std::string;
using std::vector;

using std::tr1::bind;


namespace mesos { namespace internal {

// The scheduler process (below) is responsible for interacting with
// the master and responding to Mesos API calls from scheduler
// drivers. In order to allow a message to be sent back to the master
// we allow friend functions to invoke 'send', 'post', etc. Therefore,
// we must make sure that any necessary synchronization is performed.

class SchedulerProcess : public MesosProcess<SchedulerProcess>
{
public:
  SchedulerProcess(MesosSchedulerDriver* _driver, Scheduler* _sched,
		   const FrameworkID& _frameworkId,
                   const FrameworkInfo& _framework)
    : driver(_driver), sched(_sched), frameworkId(_frameworkId),
      framework(_framework), generation(0), master(UPID()), terminate(false)
  {
    install(NEW_MASTER_DETECTED, &SchedulerProcess::newMasterDetected,
            &NewMasterDetectedMessage::pid);

    install(NO_MASTER_DETECTED, &SchedulerProcess::noMasterDetected);

    install(MASTER_DETECTION_FAILURE, &SchedulerProcess::masterDetectionFailure);

    install(M2F_REGISTER_REPLY, &SchedulerProcess::registerReply,
            &FrameworkRegisteredMessage::framework_id);

    install(M2F_RESOURCE_OFFER, &SchedulerProcess::resourceOffer,
            &ResourceOfferMessage::offer_id,
            &ResourceOfferMessage::offers,
            &ResourceOfferMessage::pids);

    install(M2F_RESCIND_OFFER, &SchedulerProcess::rescindOffer,
            &RescindResourceOfferMessage::offer_id);

    install(M2F_STATUS_UPDATE, &SchedulerProcess::statusUpdate,
            &StatusUpdateMessage::framework_id,
            &StatusUpdateMessage::status);

    install(M2F_LOST_SLAVE, &SchedulerProcess::lostSlave,
            &LostSlaveMessage::slave_id);

    install(M2F_FRAMEWORK_MESSAGE, &SchedulerProcess::frameworkMessage,
            &FrameworkMessageMessage::message);

    install(M2F_ERROR, &SchedulerProcess::error,
            &FrameworkErrorMessage::code,
            &FrameworkErrorMessage::message);

    install(process::EXITED, &SchedulerProcess::exited);
  }

  virtual ~SchedulerProcess() {}

protected:
  virtual void operator () ()
  {
    while (true) {
      // Sending a message to terminate this process is insufficient
      // because that message might get queued behind a bunch of other
      // message. So, when it is time to terminate, we set a flag that
      // gets re-read by this process after every message. In order to
      // get this correct we must return from each invocation of
      // 'serve', to check and see if terminate has been set. In
      // addition, we need to send a dummy message right after we set
      // terminate just in case there aren't any messages in the
      // queue. Note that the terminate field is only read by this
      // process, so we don't need to protect it in any way. In fact,
      // using a lock to protect it (or for providing atomicity for
      // cleanup, for example), might lead to deadlock with the client
      // code because we already use a lock in SchedulerDriver. That
      // being said, for now we make terminate 'volatile' to guarantee
      // that each read is getting a fresh copy.
      // TODO(benh): Do a coherent read so as to avoid using
      // 'volatile'.
      if (terminate) return;

      serve(0, true);
    }
  }

  void newMasterDetected(const string& pid)
  {
    VLOG(1) << "New master at " << pid;

    master = pid;
    link(master);

    if (frameworkId == "") {
      // Touched for the very first time.
      MSG<F2M_REGISTER_FRAMEWORK> out;
      out.mutable_framework()->MergeFrom(framework);
      send(master, out);
    } else {
      // Not the first time, or failing over.
      MSG<F2M_REREGISTER_FRAMEWORK> out;
      out.mutable_framework()->MergeFrom(framework);
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.set_generation(generation++);
      send(master, out);
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

  void masterDetectionFailure()
  {
    VLOG(1) << "Master detection failed";
    active = false;
    // TODO(benh): Better error codes/messages!
    int32_t code = 1;
    const string& message = "Failed to detect master(s)";
    process::invoke(bind(&Scheduler::error, sched, driver, code,
                         cref(message)));
  }

  void registerReply(const FrameworkID& frameworkId)
  {
    VLOG(1) << "Framework registered with " << frameworkId;
    this->frameworkId = frameworkId;
    process::invoke(bind(&Scheduler::registered, sched, driver,
                         cref(frameworkId)));
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
      CHECK(pid != UPID());
      savedOffers[offerId][offers[i].slave_id()] = pid;
    }

    process::invoke(bind(&Scheduler::resourceOffer, sched, driver,
                         cref(offerId), cref(offers)));
  }

  void rescindOffer(const OfferID& offerId)
  {
    VLOG(1) << "Rescinded offer " << offerId;
    savedOffers.erase(offerId);
    process::invoke(bind(&Scheduler::offerRescinded, sched, driver, 
                         cref(offerId)));
  }

  void statusUpdate(const FrameworkID& frameworkId, const TaskStatus& status)
  {
    VLOG(1) << "Status update: task " << status.task_id()
            << " of framework " << frameworkId
            << " is now in state "
            << TaskState_descriptor()->FindValueByNumber(status.state())->name();

    CHECK(this->frameworkId == frameworkId);

    // TODO(benh): Note that this maybe a duplicate status update!
    // Once we get support to try and have a more consistent view
    // of what's running in the cluster, we'll just let this one
    // slide. The alternative is possibly dealing with a scheduler
    // failover and not correctly giving the scheduler it's status
    // update, which seems worse than giving a status update
    // multiple times (of course, if a scheduler re-uses a TaskID,
    // that could be bad.

    process::invoke(bind(&Scheduler::statusUpdate, sched, driver,
                         cref(status)));

    // Acknowledge the message (we do this last, after we process::invoked
    // the scheduler, if we did at all, in case it causes a crash,
    // since this way the message might get resent/routed after
    // the scheduler comes back online).
    MSG<F2M_STATUS_UPDATE_ACK> out;
    out.mutable_framework_id()->MergeFrom(frameworkId);
    out.mutable_slave_id()->MergeFrom(status.slave_id());
    out.mutable_task_id()->MergeFrom(status.task_id());
    send(master, out);
  }

  void lostSlave(const SlaveID& slaveId)
  {
    VLOG(1) << "Lost slave " << slaveId;
    savedSlavePids.erase(slaveId);
    process::invoke(bind(&Scheduler::slaveLost, sched, driver, cref(slaveId)));
  }

  void frameworkMessage(const FrameworkMessage& message)
  {
    VLOG(1) << "Received message";
    process::invoke(bind(&Scheduler::frameworkMessage, sched, driver,
                         cref(message)));
  }

  void error(int32_t code, const string& message)
  {
    VLOG(1) << "Got error '" << message << "' (code: " << code << ")";
    process::invoke(bind(&Scheduler::error, sched, driver, code,
                         cref(message)));
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
    if (!active)
      return;

    MSG<F2M_UNREGISTER_FRAMEWORK> out;
    out.mutable_framework_id()->MergeFrom(frameworkId);
    send(master, out);
  }

  void killTask(const TaskID& taskId)
  {
    if (!active)
      return;

    MSG<F2M_KILL_TASK> out;
    out.mutable_framework_id()->MergeFrom(frameworkId);
    out.mutable_task_id()->MergeFrom(taskId);
    send(master, out);
  }

  void replyToOffer(const OfferID& offerId,
                    const vector<TaskDescription>& tasks,
                    const map<string, string>& params)
  {
    if (!active)
      return;

    MSG<F2M_RESOURCE_OFFER_REPLY> out;
    out.mutable_framework_id()->MergeFrom(frameworkId);
    out.mutable_offer_id()->MergeFrom(offerId);

    foreachpair (const string& key, const string& value, params) {
      Param* param = out.mutable_params()->add_param();
      param->set_key(key);
      param->set_value(value);
    }

    foreach (const TaskDescription& task, tasks) {
      // Keep only the slave PIDs where we run tasks so we can send
      // framework messages directly.
      savedSlavePids[task.slave_id()] = savedOffers[offerId][task.slave_id()];

      out.add_tasks()->MergeFrom(task);
    }

    // Remove the offer since we saved all the PIDs we might use.
    savedOffers.erase(offerId);

    send(master, out);
  }

  void reviveOffers()
  {
    if (!active)
      return;

    MSG<F2M_REVIVE_OFFERS> out;
    out.mutable_framework_id()->MergeFrom(frameworkId);
    send(master, out);
  }

  void sendFrameworkMessage(const FrameworkMessage& message)
  {
    if (!active)
      return;

    VLOG(1) << "Asked to send framework message to slave "
	    << message.slave_id();

    // TODO(benh): After a scheduler has re-registered it won't have
    // any saved slave PIDs, maybe it makes sense to try and save each
    // PID that this scheduler tries to send a message to? Or we can
    // just wait for them to recollect as new offers come in and get
    // accepted.

    if (savedSlavePids.count(message.slave_id()) > 0) {
      UPID slave = savedSlavePids[message.slave_id()];
      CHECK(slave != UPID());

      // TODO(benh): This is kind of wierd, M2S?
      MSG<M2S_FRAMEWORK_MESSAGE> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_message()->MergeFrom(message);
      send(slave, out);
    } else {
      VLOG(1) << "Cannot send directly to slave " << message.slave_id()
	      << "; sending through master";

      MSG<F2M_FRAMEWORK_MESSAGE> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_message()->MergeFrom(message);
      send(master, out);
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
  volatile bool terminate;

  unordered_map<OfferID, unordered_map<SlaveID, UPID> > savedOffers;
  unordered_map<SlaveID, UPID> savedSlavePids;
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
    process::wait(process->self());
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

  UPID pid = process::spawn(process);

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
    process::dispatch(process->self(), &SchedulerProcess::stop);
    process->terminate = true;
    process::post(process->self(), process::TERMINATE);
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

  process::dispatch(process->self(), &SchedulerProcess::killTask,
                    taskId);

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

  process::dispatch(process->self(), &SchedulerProcess::replyToOffer,
                    offerId, tasks, params);

  return 0;
}


int MesosSchedulerDriver::reviveOffers()
{
  Lock lock(&mutex);

  if (!running || (process != NULL && !process->active)) {
    return -1;
  }

  process::dispatch(process->self(), &SchedulerProcess::reviveOffers);

  return 0;
}


int MesosSchedulerDriver::sendFrameworkMessage(const FrameworkMessage& message)
{
  Lock lock(&mutex);

  if (!running || (process != NULL && !process->active)) {
    return -1;
  }

  // Make sure necessary fields have been completed.
  if (!message.has_slave_id()) {
    VLOG(1) << "Missing SlaveID (slave_id), cannot send message";
    return -1;
  }

  if (!message.has_executor_id()) {
    VLOG(1) << "Missing ExecutorID (executor_id), cannot send message";
    return -1;
  }

  process::dispatch(process->self(), &SchedulerProcess::sendFrameworkMessage,
                    message);

  return 0;
}


void MesosSchedulerDriver::error(int code, const string& message)
{
  sched->error(this, code, message);
}
