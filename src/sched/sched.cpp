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

#include <mesos.hpp>
#include <mesos_sched.hpp>
#include <reliable.hpp>

#include <boost/bind.hpp>
#include <boost/unordered_map.hpp>

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

using std::map;
using std::string;
using std::vector;

using boost::bind;
using boost::cref;
using boost::unordered_map;


namespace mesos { namespace internal {


// Unfortunately, when we reply to an offer right now the message
// might not make it to the master, or even all the way to the
// slave. So, we preemptively assume the task has been lost if we
// don't here from it after some timeout (see below). TODO(benh):
// Eventually, what we would like to do is actually query this state
// in the master, or possibly even re-launch the task.

#define STATUS_UPDATE_TIMEOUT 20

class StatusUpdateTimer : public MesosProcess
{
public:
  StatusUpdateTimer(const PID &_sched, const FrameworkID& _frameworkId,
                    const TaskDescription& task)
    : sched(_sched), frameworkId(_frameworkId), taskId(task.task_id()),
      slaveId(task.slave_id()), terminate(false) {}
  
protected:
  virtual void operator () ()
  {
    link(sched);
    while (!terminate) {
      switch (receive(STATUS_UPDATE_TIMEOUT)) {
        case PROCESS_TIMEOUT: {
          terminate = true;
          VLOG(1) << "No status updates received for task ID: "
                  << taskId << " after "
                  << STATUS_UPDATE_TIMEOUT << ", assuming task was lost";
          Message<M2F_STATUS_UPDATE> out;
          *out.mutable_framework_id() = frameworkId;
          TaskStatus* status = out.mutable_status();
          *status->mutable_task_id() = taskId;
          *status->mutable_slave_id() = slaveId;
          status->set_state(TASK_LOST);
          send(sched, out);
          break;
        }
        default: {
          terminate = true;
          break;
        }
      }
    }
  }

private:
  const PID sched;
  const FrameworkID frameworkId;
  const TaskID taskId;
  const SlaveID slaveId;
  bool terminate;
};


// The scheduler process (below) is responsible for interacting with
// the master and responding to Mesos API calls from scheduler
// drivers. In order to allow a message to be sent back to the master
// we allow friend functions to invoke 'send', 'post', etc. Therefore,
// we must make sure that any necessary synchronization is performed.

class SchedulerProcess : public MesosProcess
{
public:
  SchedulerProcess(MesosSchedulerDriver* _driver, Scheduler* _sched,
		   const FrameworkID& _frameworkId,
                   const FrameworkInfo& _framework)
    : driver(_driver), sched(_sched), frameworkId(_frameworkId),
      framework(_framework), generation(0), master(PID()), terminate(false) {}

  ~SchedulerProcess()
  {
    // Cleanup any remaining timers.
    foreachpair (const TaskID& taskId, StatusUpdateTimer* timer, timers) {
      send(timer->self(), MESOS_MSGID);
      wait(timer->self());
      delete timer;
    }
  }

protected:
  virtual void operator () ()
  {
    while (true) {
      // Rather than send a message to this process when it is time to
      // terminate, we set a flag that gets re-read. Sending a message
      // requires some sort of matching or priority reads that
      // libprocess currently doesn't support. Note that this field is
      // only read by this process, so we don't need to protect it in
      // any way. In fact, using a lock to protect it (or for
      // providing atomicity for cleanup, for example), might lead to
      // deadlock with the client code because we already use a lock
      // in SchedulerDriver. That being said, for now we make
      // terminate 'volatile' to guarantee that each read is getting a
      // fresh copy.
      // TODO(benh): Do a coherent read so as to avoid using 'volatile'.
      if (terminate)
        return;

      // TODO(benh): We need to break the receive every so often to
      // check if 'terminate' has been set. It would be better to just
      // send a message rather than have a timeout (see the comment
      // above for why sending a message will still require us to use
      // the terminate flag).
      switch (serve(2)) {

      case NEW_MASTER_DETECTED: {
        const Message<NEW_MASTER_DETECTED>& msg = message();

        VLOG(1) << "New master at " << msg.pid();

        redirect(master, msg.pid());
        master = msg.pid();
        link(master);

        if (frameworkId == "") {
          // Touched for the very first time.
          Message<F2M_REGISTER_FRAMEWORK> out;
          *out.mutable_framework() = framework;
          send(master, out);
        } else {
          // Not the first time, or failing over.
          Message<F2M_REREGISTER_FRAMEWORK> out;
          *out.mutable_framework() = framework;
          *out.mutable_framework_id() = frameworkId;
          out.set_generation(generation++);
          send(master, out);
        }

	active = true;

	break;
      }

      case NO_MASTER_DETECTED: {
	// In this case, we don't actually invoke Scheduler::error
	// since we might get reconnected to a master imminently.
	active = false;
	VLOG(1) << "No master detected, waiting for another master";
	break;
      }

      case MASTER_DETECTION_FAILURE: {
	active = false;
	// TODO(benh): Better error codes/messages!
        int32_t code = 1;
        const string& message = "Failed to detect master(s)";
        invoke(bind(&Scheduler::error, sched, driver, code, cref(message)));
	break;
      }

      case M2F_REGISTER_REPLY: {
        const Message<M2F_REGISTER_REPLY>& msg = message();
        frameworkId = msg.framework_id();
        invoke(bind(&Scheduler::registered, sched, driver, cref(frameworkId)));
        break;
      }

      case M2F_RESOURCE_OFFER: {
        const Message<M2F_RESOURCE_OFFER>& msg = message();

        // Construct a vector for the offers. Also save the pid
        // associated with each slave (one per SlaveOffer) so later we
        // can send framework messages directly.
        vector<SlaveOffer> offers;

        for (int i = 0; i < msg.offer_size(); i++) {
          const SlaveOffer& offer = msg.offer(i);
          PID pid(msg.pid(i));
          CHECK(pid != PID());
          savedOffers[msg.offer_id()][offer.slave_id()] = pid;
          offers.push_back(offer);
        }

        invoke(bind(&Scheduler::resourceOffer, sched, driver,
                    cref(msg.offer_id()), cref(offers)));
        break;
      }

      case M2F_RESCIND_OFFER: {
        const Message<M2F_RESCIND_OFFER>& msg = message();
        savedOffers.erase(msg.offer_id());
        invoke(bind(&Scheduler::offerRescinded, sched, driver,
                    cref(msg.offer_id())));
        break;
      }

      case M2F_STATUS_UPDATE: {
        const Message<M2F_STATUS_UPDATE>& msg = message();

        const TaskStatus &status = msg.status();

        // Drop this if it's a duplicate.
        if (duplicate()) {
          VLOG(1) << "Received a duplicate status update for task "
                  << status.task_id() << ", status = " << status.state();
          break;
        }

        // Stop any status update timers we might have had running.
        if (timers.count(status.task_id()) > 0) {
          StatusUpdateTimer* timer = timers[status.task_id()];
          timers.erase(status.task_id());
          send(timer->self(), MESOS_MSGID);
          wait(timer->self());
          delete timer;
        }

        invoke(bind(&Scheduler::statusUpdate, sched, driver, cref(status)));

        // Acknowledge the message (we do this after we invoke the
        // scheduler in case it causes a crash, since this way the
        // message might get resent/routed after the scheduler comes
        // back online).
        ack();
        break;
      }

      case M2F_FRAMEWORK_MESSAGE: {
        const Message<M2F_FRAMEWORK_MESSAGE>& msg = message();
        invoke(bind(&Scheduler::frameworkMessage, sched, driver,
                    cref(msg.message())));
        break;
      }

      case M2F_LOST_SLAVE: {
        const Message<M2F_LOST_SLAVE>& msg = message();
	savedSlavePids.erase(msg.slave_id());
        invoke(bind(&Scheduler::slaveLost, sched, driver,
                    cref(msg.slave_id())));
        break;
      }

      case M2F_ERROR: {
        const Message<M2F_ERROR>& msg = message();
        int32_t code = msg.code();
        const string& message = msg.message();
        invoke(bind(&Scheduler::error, sched, driver, code, cref(message)));
        break;
      }

      case PROCESS_EXIT: {
	// TODO(benh): Don't wait for a new master forever.
        if (from() == master)
          VLOG(1) << "Connection to master lost .. waiting for new master";
        break;
      }

      case PROCESS_TIMEOUT: {
        break;
      }

      default: {
        VLOG(1) << "Received unknown message " << msgid()
                << " from " << from();
        break;
      }
      }
    }
  }

  void stop()
  {
    if (!active)
      return;

    Message<F2M_UNREGISTER_FRAMEWORK> out;
    *out.mutable_framework_id() = frameworkId;
    send(master, out);
  }

  void killTask(const TaskID& taskId)
  {
    if (!active)
      return;

    Message<F2M_KILL_TASK> out;
    *out.mutable_framework_id() = frameworkId;
    *out.mutable_task_id() = taskId;
    send(master, out);
  }

  void replyToOffer(const OfferID& offerId,
                    const vector<TaskDescription>& tasks,
                    const map<string, string>& params)
  {
    if (!active)
      return;

    Message<F2M_RESOURCE_OFFER_REPLY> out;
    *out.mutable_framework_id() = frameworkId;
    *out.mutable_offer_id() = offerId;

    foreachpair (const string& key, const string& value, params) {
      Param* param = out.mutable_params()->add_param();
      param->set_key(key);
      param->set_value(value);
    }

    foreach (const TaskDescription& task, tasks) {
      // Keep only the slave PIDs where we run tasks so we can send
      // framework messages directly.
      savedSlavePids[task.slave_id()] = savedOffers[offerId][task.slave_id()];

      // Create timers to ensure we get status updates for these tasks.
      StatusUpdateTimer *timer = new StatusUpdateTimer(self(), frameworkId, task);
      timers[task.task_id()] = timer;
      spawn(timer);

      // Copy the task over.
      *(out.add_task()) = task;
    }

    // Remove the offer since we saved all the PIDs we might use.
    savedOffers.erase(offerId);

    send(master, out);
  }

  void reviveOffers()
  {
    if (!active)
      return;

    Message<F2M_REVIVE_OFFERS> out;
    *out.mutable_framework_id() = frameworkId;
    send(master, out);
  }

  void sendFrameworkMessage(const FrameworkMessage& message)
  {
    if (!active)
      return;

    VLOG(1) << "Asked to send framework message to slave "
	    << message.slave_id();

    // TODO(benh): This is kind of wierd, M2S?
    Message<M2S_FRAMEWORK_MESSAGE> out;
    *out.mutable_framework_id() = frameworkId;
    *out.mutable_message() = message;

    if (savedSlavePids.count(message.slave_id()) > 0) {
      PID slave = savedSlavePids[message.slave_id()];
      CHECK(slave != PID());
      send(slave, out);
    } else {
      VLOG(1) << "Cannot send directly to slave " << message.slave_id()
	      << "; sending through master";
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
  PID master;

  volatile bool active;
  volatile bool terminate;

  unordered_map<OfferID, unordered_map<SlaveID, PID> > savedOffers;
  unordered_map<SlaveID, PID> savedSlavePids;

  // Timers to ensure we get a status update for each task we launch.
  unordered_map<TaskID, StatusUpdateTimer *> timers;
};

}} /* namespace mesos { namespace internal { */


/*
 * Implementation of C++ API.
 *
 * Notes:
 *
 * (1) Callbacks should be serialized as well as calls into the
 *     class. We do the former because the message reads from
 *     SchedulerProcess are serialized. We do the latter currently by
 *     using locks for certain methods ... but this may change in the
 *     future.
 *
 * (2) There are two possible status variables, one called 'active' in
       SchedulerProcess and one called 'running' in
       MesosSchedulerDriver. The former is used to represent whether
       or not we are connected to an active master while the latter is
       used to represent whether or not a client has called
       MesosSchedulerDriver::start/run or MesosSchedulerDriver::stop.
 */


MesosSchedulerDriver::MesosSchedulerDriver(Scheduler* sched,
					   const string& url,
					   const FrameworkID& frameworkId)
{
  Configurator configurator;
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
  // deadlock implies that the destructor got called from within a method
  // of the Scheduler instance that is being destructed! Note
  // that we could add a method to libprocess that told us whether or
  // not this was about to be deadlock, and possibly report this back
  // to the user somehow.
  if (process != NULL) {
    Process::wait(process->self());
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
  if (url == "local" || url == "localquiet")
    local::shutdown();
}


int MesosSchedulerDriver::start()
{
  Lock lock(&mutex);

  if (running) {
    return -1;
  }

  // Set running here so we can recognize an exception from calls into
  // Java (via getFrameworkName or getExecutorInfo).
  running = true;

  // Get username of current user.
  passwd* passwd;
  if ((passwd = getpwuid(getuid())) == NULL)
    fatal("failed to get username information");

  // Set up framework info.
  FrameworkInfo framework;
  framework.set_user(passwd->pw_name);
  framework.set_name(sched->getFrameworkName(this));
  *framework.mutable_executor() = sched->getExecutorInfo(this);

  // Something invoked stop while we were in the scheduler, bail.
  if (!running)
    return -1;

  process = new SchedulerProcess(this, sched, frameworkId, framework);

  PID pid = Process::spawn(process);

  // Check and see if we need to launch a local cluster.
  if (url == "local") {
    PID master = local::launch(*conf, true);
    detector = new BasicMasterDetector(master, pid);
  } else if (url == "localquiet") {
    conf->set("quiet", 1);
    PID master = local::launch(*conf, true);
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
    Process::dispatch(process, &SchedulerProcess::stop);
    process->terminate = true;
    process = NULL;
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

  while (running)
    pthread_cond_wait(&cond, &mutex);

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

  Process::dispatch(process, &SchedulerProcess::killTask, taskId);

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

  Process::dispatch(process, &SchedulerProcess::replyToOffer,
                    offerId, tasks, params);

  return 0;
}


int MesosSchedulerDriver::reviveOffers()
{
  Lock lock(&mutex);

  if (!running || (process != NULL && !process->active)) {
    return -1;
  }

  Process::dispatch(process, &SchedulerProcess::reviveOffers);

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

  Process::dispatch(process, &SchedulerProcess::sendFrameworkMessage, message);

  return 0;
}


void MesosSchedulerDriver::error(int code, const string& message)
{
  sched->error(this, code, message);
}
