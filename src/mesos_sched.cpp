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

#include <reliable.hpp>

#include <boost/bind.hpp>
#include <boost/unordered_map.hpp>

#include "mesos_sched.h"

#include "fatal.hpp"
#include "lock.hpp"
#include "logging.hpp"
#include "master_detector.hpp"
#include "messages.hpp"
#include "mesos_local.hpp"
#include "mesos_sched.hpp"

#define REPLY_TIMEOUT 20

using std::cerr;
using std::cout;
using std::endl;
using std::map;
using std::string;
using std::vector;

using boost::bind;
using boost::ref;
using boost::unordered_map;

using namespace mesos;
using namespace mesos::internal;
using mesos::internal::master::Master;
using mesos::internal::slave::Slave;


namespace mesos { namespace internal {


class RbReply : public MesosProcess
{    
public:
  RbReply(const PID &_p, const TaskID &_tid) : 
    parent(_p), tid(_tid), terminate(false) {}
  
protected:
  void operator () ()
  {
    link(parent);
    while(!terminate) {

      switch(receive(REPLY_TIMEOUT)) {
      case F2F_TASK_RUNNING_STATUS: {
        terminate = true;
        break;
      }

      case PROCESS_TIMEOUT: {
        terminate = true;
        VLOG(1) << "FT: faking M2F_STATUS_UPDATE due to ReplyToOffer timeout for tid:" << tid;
        send(parent, pack<M2F_STATUS_UPDATE>(tid, TASK_LOST, ""));
        break;
      }

      }
    }
    VLOG(1) << "FT: Exiting reliable reply for tid:" << tid;
  }

private:
  bool terminate;
  const PID parent;
  const TaskID tid;
};


/**
 * TODO(benh): Update this comment.
 * Scheduler process, responsible for interacting with the master
 * and responding to Mesos API calls from schedulers. In order to
 * allow a message to be sent back to the master we allow friend
 * functions to invoke 'send'. Therefore, care must be done to insure
 * any synchronization necessary is performed.
 */
class SchedulerProcess : public MesosProcess
{
public:
  friend class mesos::MesosSchedulerDriver;

private:
  MesosSchedulerDriver* driver;
  Scheduler* sched;
  FrameworkID fid;
  string frameworkName;
  ExecutorInfo execInfo;
  int32_t generation;
  PID master;

  volatile bool terminate;

  unordered_map<OfferID, unordered_map<SlaveID, PID> > savedOffers;
  unordered_map<SlaveID, PID> savedSlavePids;

  unordered_map<TaskID, RbReply *> rbReplies;

public:
  SchedulerProcess(MesosSchedulerDriver* _driver,
                   Scheduler* _sched,
		   FrameworkID _fid,
                   const string& _frameworkName,
                   const ExecutorInfo& _execInfo)
    : driver(_driver),
      sched(_sched),
      fid(_fid),
      frameworkName(_frameworkName),
      execInfo(_execInfo),
      generation(0),
      master(PID()),
      terminate(false) {}

  ~SchedulerProcess() {}

protected:
  void operator () ()
  {
    // Get username of current user.
    struct passwd* passwd;
    if ((passwd = getpwuid(getuid())) == NULL)
      fatal("MesosSchedulerDriver failed to get username information");

    const string user(passwd->pw_name);

    while(true) {
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
      switch(receive(2)) {

      case NEW_MASTER_DETECTED: {
	string masterSeq;
	PID masterPid;
	tie(masterSeq, masterPid) = unpack<NEW_MASTER_DETECTED>(body());

	LOG(INFO) << "New master at " << masterPid << " with ID:" << masterSeq;

        redirect(master, masterPid);
	master = masterPid;
	link(master);

	if (fid == "") {
	  // Touched for the very first time.
	  send(master, pack<F2M_REGISTER_FRAMEWORK>(frameworkName, user, execInfo));
	} else {
	  // Not the first time, or failing over.
	  send(master, pack<F2M_REREGISTER_FRAMEWORK>(fid, frameworkName, user,
						      execInfo, generation++));
	}
	break;
      }

      case NO_MASTER_DETECTED: {
	// TODO(benh): We need to convey to the driver that we are
	// currently not connected (not running?) so that calls such
	// as sendFrameworkMessage will return with an error (rather
	// than just getting dropped all together).
	break;
      }

      case M2F_REGISTER_REPLY: {
        tie(fid) = unpack<M2F_REGISTER_REPLY>(body());
        invoke(bind(&Scheduler::registered, sched, driver, fid));
        break;
      }

      case M2F_SLOT_OFFER: {
        OfferID oid;
        vector<SlaveOffer> offs;
        tie(oid, offs) = unpack<M2F_SLOT_OFFER>(body());
        
	// Save all the slave PIDs found in the offer so later we can
	// send framework messages directly.
        foreach(const SlaveOffer &offer, offs) {
	  savedOffers[oid][offer.slaveId] = offer.slavePid;
        }

        invoke(bind(&Scheduler::resourceOffer, sched, driver, oid, ref(offs)));
        break;
      }

      case F2F_SLOT_OFFER_REPLY: {
        OfferID oid;
        vector<TaskDescription> tasks;
        Params params;
        tie(oid, tasks, params) = unpack<F2F_SLOT_OFFER_REPLY>(body());

	// Keep only the slave PIDs where we run tasks so we can send
	// framework messages directly.
        foreach(const TaskDescription &task, tasks) {
          savedSlavePids[task.slaveId] = savedOffers[oid][task.slaveId];
        }

	// Remove the offer since we saved all the PIDs we might use.
        savedOffers.erase(oid);

	foreach(const TaskDescription &task, tasks) {
	  RbReply *rr = new RbReply(self(), task.taskId);
	  rbReplies[task.taskId] = rr;
	  // TODO(benh): Link?
	  spawn(rr);
	}
        
        send(master, pack<F2M_SLOT_OFFER_REPLY>(fid, oid, tasks, params));
        break;
      }

      case F2F_FRAMEWORK_MESSAGE: {
        FrameworkMessage msg;
        tie(msg) = unpack<F2F_FRAMEWORK_MESSAGE>(body());
        send(savedSlavePids[msg.slaveId], pack<M2S_FRAMEWORK_MESSAGE>(fid, msg));
        break;
      }

      case M2F_RESCIND_OFFER: {
        OfferID oid;
        tie(oid) = unpack<M2F_RESCIND_OFFER>(body());
        savedOffers.erase(oid);
        invoke(bind(&Scheduler::offerRescinded, sched, driver, oid));
        break;
      }

	// TODO(benh): Fix forwarding issues.
//       case M2F_FT_STATUS_UPDATE: {
//         TaskID tid;
//         TaskState state;
//         string data;
//         unpack<M2F_FT_STATUS_UPDATE>(tid, state, data);
      case S2M_FT_STATUS_UPDATE: {
	SlaveID sid;
	FrameworkID fid;
	TaskID tid;
	TaskState state;
	string data;

	tie(sid, fid, tid, state, data) = unpack<S2M_FT_STATUS_UPDATE>(body());

        if (duplicate())
          break;
        ack();
        VLOG(1) << "FT: Received message with id: " << seq();

	unordered_map <TaskID, RbReply *>::iterator it = rbReplies.find(tid);
	if (it != rbReplies.end()) {
	  RbReply *rr = it->second;
	  send(rr->self(), pack<F2F_TASK_RUNNING_STATUS>());
	  wait(rr->self());
	  rbReplies.erase(tid);
	  delete rr;
	}

        TaskStatus status(tid, state, data);
        invoke(bind(&Scheduler::statusUpdate, sched, driver, ref(status)));
        break;
      }

      case M2F_STATUS_UPDATE: {
        TaskID tid;
        TaskState state;
        string data;
        tie(tid, state, data) = unpack<M2F_STATUS_UPDATE>(body());

	unordered_map <TaskID, RbReply *>::iterator it = rbReplies.find(tid);
	if (it != rbReplies.end()) {
	  RbReply *rr = it->second;
	  send(rr->self(), pack<F2F_TASK_RUNNING_STATUS>());
	  wait(rr->self());
	  rbReplies.erase(tid);
	  delete rr;
	}

        TaskStatus status(tid, state, data);
        invoke(bind(&Scheduler::statusUpdate, sched, driver, ref(status)));
        break;
      }

      case M2F_FRAMEWORK_MESSAGE: {
        FrameworkMessage msg;
        tie(msg) = unpack<M2F_FRAMEWORK_MESSAGE>(body());
        invoke(bind(&Scheduler::frameworkMessage, sched, driver, ref(msg)));
        break;
      }

      case M2F_LOST_SLAVE: {
        SlaveID sid;
        tie(sid) = unpack<M2F_LOST_SLAVE>(body());
	savedSlavePids.erase(sid);
        invoke(bind(&Scheduler::slaveLost, sched, driver, sid));
        break;
      }

      case M2F_ERROR: {
        int32_t code;
        string message;
        tie(code, message) = unpack<M2F_ERROR>(body());
        invoke(bind(&Scheduler::error, sched, driver, code, ref(message)));
        break;
      }

      case PROCESS_EXIT: {
	// TODO(benh): Don't wait for a new master forever.
	LOG(WARNING) << "Connection to master lost .. waiting for new master.";
        break;
      }

      case PROCESS_TIMEOUT: {
        break;
      }

      default: {
        ostringstream oss;
        oss << "SchedulerProcess received unknown message " << msgid()
            << " from " << from() << endl;
        invoke(bind(&Scheduler::error, sched, driver, -1, oss.str()));
        break;
      }
      }
    }
  }
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
 */


/**
 * Default implementations of getFrameworkName that returns a dummy
 * name. We need this because SWIG wouldn't be happy if getFrameworkName
 * was a pure virtual method.
 */
string Scheduler::getFrameworkName(SchedulerDriver*)
{
  return "Unnamed Framework";
}


/**
 * Default implementations of getExecutorInfo that returns a dummy
 * name. We need this because SWIG wouldn't be happy if getExecutorInfo
 * was a pure virtual method.
 */
ExecutorInfo Scheduler::getExecutorInfo(SchedulerDriver*)
{
  return ExecutorInfo("", "");
}


// Default implementation of Scheduler::error that logs to stderr
void Scheduler::error(SchedulerDriver* driver, int code, const string &message)
{
  cerr << "Mesos error: " << message
       << " (error code: " << code << ")" << endl;
  driver->stop();
}


MesosSchedulerDriver::MesosSchedulerDriver(Scheduler* sched,
					   const string &url,
					   FrameworkID fid)
{
  Configurator configurator;
  local::registerOptions(&configurator);
  Params* conf;
  try {
    conf = new Params(configurator.load());
  } catch (ConfigurationException& e) {
    string message = string("Configuration error: ") + e.what();
    sched->error(this, 2, message);
    conf = new Params();
  }
  conf->set("url", url); // Override URL param with the one from the user
  init(sched, conf, fid);
}


MesosSchedulerDriver::MesosSchedulerDriver(Scheduler* sched,
					   const string_map &params,
					   FrameworkID fid)
{
  Configurator configurator;
  local::registerOptions(&configurator);
  try {
    conf = new Params(configurator.load(params));
  } catch (ConfigurationException& e) {
    string message = string("Configuration error: ") + e.what();
    sched->error(this, 2, message);
    conf = new Params();
  }
  init(sched, conf, fid);
}


MesosSchedulerDriver::MesosSchedulerDriver(Scheduler* sched,
					   int argc,
                                           char** argv,
					   FrameworkID fid)
{
  Configurator configurator;
  local::registerOptions(&configurator);
  try {
    conf = new Params(configurator.load(argc, argv, false));
  } catch (ConfigurationException& e) {
    string message = string("Configuration error: ") + e.what();
    sched->error(this, 2, message);
    conf = new Params();
  }
  init(sched, conf, fid);
}


void MesosSchedulerDriver::init(Scheduler* _sched,
                                Params* _conf,
                                FrameworkID _fid)
{
  sched = _sched;
  conf = _conf;
  fid = _fid;
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
    //error(1, "cannot call start - scheduler is already running");
    return - 1;
  }

  const string& frameworkName = sched->getFrameworkName(this);
  const ExecutorInfo& executorInfo = sched->getExecutorInfo(this);

  process = new SchedulerProcess(this, sched, fid, frameworkName, executorInfo);
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
    detector = MasterDetector::create(url, pid, false, true);
  }

  running = true;

  return 0;
}


int MesosSchedulerDriver::stop()
{
  Lock lock(&mutex);

  if (!running) {
    // Don't issue an error (could lead to an infinite loop).
    return -1;
  }

  // TODO(benh): Do a Process::post instead?
  process->send(process->master,
                pack<F2M_UNREGISTER_FRAMEWORK>(process->fid));

  process->terminate = true;

  running = false;

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


int MesosSchedulerDriver::killTask(TaskID tid)
{
  Lock lock(&mutex);

  if (!running) {
    //error(1, "cannot call killTask - scheduler is not running");
    return -1;
  }

  // TODO(benh): Do a Process::post instead?

  process->send(process->master,
                pack<F2M_KILL_TASK>(process->fid, tid));

  return 0;
}


int MesosSchedulerDriver::replyToOffer(OfferID offerId,
				       const vector<TaskDescription> &tasks,
				       const string_map &params)
{
  Lock lock(&mutex);

  if (!running) {
    //error(1, "cannot call replyToOffer - scheduler is not running");
    return -1;
  }

  // TODO(benh): Do a Process::post instead?
  
  process->send(process->self(),
                pack<F2F_SLOT_OFFER_REPLY>(offerId, tasks, Params(params)));

  return 0;
}


int MesosSchedulerDriver::reviveOffers()
{
  Lock lock(&mutex);

  if (!running) {
    //error(1, "cannot call reviveOffers - scheduler is not running");
    return -1;
  }

  // TODO(benh): Do a Process::post instead?

  process->send(process->master,
                pack<F2M_REVIVE_OFFERS>(process->fid));

  return 0;
}


int MesosSchedulerDriver::sendFrameworkMessage(const FrameworkMessage &message)
{
  Lock lock(&mutex);

  if (!running) {
    //error(1, "cannot call sendFrameworkMessage - scheduler is not running");
    return -1;
  }

  // TODO(benh): Do a Process::post instead?

  process->send(process->self(), pack<F2F_FRAMEWORK_MESSAGE>(message));

  return 0;
}


int MesosSchedulerDriver::sendHints(const string_map& hints)
{
  Lock lock(&mutex);

  if (!running) {
    //error(1, "cannot call sendHints - scheduler is not running");
    return -1;
  }

  // TODO(*): Send the hints; for now, we do nothing
  //error(1, "sendHints is not yet implemented");
  return -1;
}


void MesosSchedulerDriver::error(int code, const string& message)
{
  sched->error(this, code, message);
}


/*
 * Implementation of C API. We comprimise the performance of the C API
 * in favor of the performance of the C++ API by having the C API call
 * into the C++ API. Having the C++ API call into the C API is slower
 * because the rest of Mesos is written in C++, which means
 * translations are required when going from C++ to C and then back to
 * C++ when having the C++ API call into the C API.
 */


namespace mesos { namespace internal {

/*
 * We wrap calls from the C API into the C++ API with the following
 * specialized implementation of SchedulerDriver.
 */
class CScheduler : public Scheduler
{
public:
  string frameworkName;
  ExecutorInfo execInfo;
  mesos_sched* sched;
  SchedulerDriver* driver; // Set externally after object is created

  CScheduler(string fwName,
                      string execUri,
                      string execArg,
                      mesos_sched* _sched)
    : frameworkName(fwName),
      execInfo(execUri, execArg),
      sched(_sched),
      driver(NULL) {}

  virtual ~CScheduler() {}

  virtual string getFrameworkName(SchedulerDriver*)
  {
    return frameworkName;
  }

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*)
  {
    return execInfo;
  }

  virtual void registered(SchedulerDriver*, FrameworkID frameworkId)
  {
    sched->registered(sched, frameworkId.c_str());
  }

  virtual void resourceOffer(SchedulerDriver*,
                             OfferID offerId,
                             const vector<SlaveOffer> &offers)
  {
    // Convert parameters to key=value strings to give C pointers into them
    vector<string> paramStrs(offers.size());
    for (size_t i = 0; i < offers.size(); i++) {
      ostringstream oss;
      foreachpair (const string& k, const string& v, offers[i].params)
        oss << k << "=" << v << "\n";
      paramStrs[i] = oss.str();
    }

    // Create C offer structs
    mesos_slot* c_offers = new mesos_slot[offers.size()];
    for (size_t i = 0; i < offers.size(); i++) {
      mesos_slot offer = { offers[i].slaveId.c_str(),
                           offers[i].host.c_str(),
                           paramStrs[i].c_str() };
      c_offers[i] = offer;
    }

    sched->slot_offer(sched, offerId.c_str(), c_offers, offers.size());
    delete[] c_offers;
  }

  virtual void offerRescinded(SchedulerDriver*, OfferID offerId)
  {
    sched->slot_offer_rescinded(sched, offerId.c_str());
  }

  virtual void statusUpdate(SchedulerDriver*, TaskStatus &status)
  {
    mesos_task_status c_status = { status.taskId,
                                   status.state,
                                   status.data.data(),
                                   status.data.size() };
    sched->status_update(sched, &c_status);
  }

  virtual void frameworkMessage(SchedulerDriver*, FrameworkMessage &message)
  {
    mesos_framework_message c_message = { message.slaveId.c_str(),
                                          message.taskId,
                                          message.data.data(),
                                          message.data.size() };
    sched->framework_message(sched, &c_message);
  }

  virtual void slaveLost(SchedulerDriver*, SlaveID sid)
  {
    sched->slave_lost(sched, sid.c_str());
  }

  virtual void error(SchedulerDriver*, int code, const std::string &message)
  {
    sched->error(sched, code, message.c_str());
  }
};


/*
 * We record the mapping from mesos_sched to CScheduler. It would
 * be preferable to somehow link the two without requiring any extra data
 * structures, but without adding some random field to mesos_sched (or
 * doing some object prefix approach), this is the best we got.
 *
 * TODO(benh): Make access to these thread safe once we have some
 * locking mechanism that abstracts away if we are using pthreads or
 * libprocess.
 */
unordered_map<mesos_sched* , CScheduler*> c_schedulers;


CScheduler* lookupCScheduler(mesos_sched* sched)
{
  assert(sched != NULL);

  // TODO(benh): Protect 'c_schedulers' (see above).
  unordered_map<mesos_sched*, CScheduler*>::iterator it =
    c_schedulers.find(sched);

  CScheduler* cs = it == c_schedulers.end() ? NULL : it->second;

  if (cs == NULL) {
    string fw_name = sched->framework_name;
    string exec_name = sched->executor_name;
    string init_arg((char*) sched->init_arg, sched->init_arg_len);
    cs = new CScheduler(fw_name, exec_name, init_arg, sched);
    c_schedulers[sched] = cs;
  }

  assert(cs != NULL);

  return cs;
}


void removeCScheduler(mesos_sched* sched)
{
  // TODO(benh): Protect 'c_schedulers' (see above).
  unordered_map<mesos_sched*, CScheduler*>::iterator it =
    c_schedulers.find(sched);

  CScheduler* cs = it == c_schedulers.end() ? NULL : it->second;

  if (cs != NULL) {
    c_schedulers.erase(sched);
    if (cs->driver != NULL)
      delete cs->driver;
    delete cs;
  }
}


}} /* namespace mesos { namespace internal { */


extern "C" {

// TODO(*): For safety, it would be better if we allocate the
// mesos_sched objects (that way we won't ever call into them and
// segfault in our code), but that's not the C way, probably because
// it means mesos_sched objects can't get put on the stack.

int mesos_sched_init(struct mesos_sched* sched)
{
  if (sched == NULL) {
    errno = EINVAL;
    return -1;
  }

  return 0;
}


int mesos_sched_destroy(struct mesos_sched* sched)
{
  if (sched == NULL) {
    errno = EINVAL;
    return -1;
  }

  removeCScheduler(sched);

  return 0;
}


int mesos_sched_reg(struct mesos_sched* sched, const char* master)
{
  if (sched == NULL || master == NULL) {
    errno = EINVAL;
    return -1;
  }

  CScheduler* cs = lookupCScheduler(sched);

  if (cs->driver != NULL) {
    errno = EINVAL;
    return -1;
  }

  try {
    cs->driver = new MesosSchedulerDriver(cs, master);
  } catch (ConfigurationException& e) {
    string message = string("Configuration error: ") + e.what();
    sched->error(sched, 2, message.c_str());
    errno = EINVAL;
    return -2;
  }

  cs->driver->start();

  return 0;
}


int mesos_sched_reg_with_params(struct mesos_sched* sched, const char* params)
{
  if (sched == NULL || params == NULL) {
    errno = EINVAL;
    return -1;
  }

  CScheduler* cs = lookupCScheduler(sched);

  if (cs->driver != NULL) {
    errno = EINVAL;
    return -1;
  }

  try {
    Params paramsObj(params);
    cs->driver = new MesosSchedulerDriver(cs, paramsObj.getMap());
  } catch (ConfigurationException& e) {
    string message = string("Configuration error: ") + e.what();
    sched->error(sched, 2, message.c_str());
    errno = EINVAL;
    return -2;
  }

  cs->driver->start();

  return 0;
}


int mesos_sched_reg_with_cmdline(struct mesos_sched* sched,
                                 int argc,
                                 char** argv)
{
  if (sched == NULL || argc < 0 || argv == NULL) {
    errno = EINVAL;
    return -1;
  }

  CScheduler* cs = lookupCScheduler(sched);

  if (cs->driver != NULL) {
    errno = EINVAL;
    return -1;
  }

  try {
    cs->driver = new MesosSchedulerDriver(cs, argc, argv);
  } catch (ConfigurationException& e) {
    string message = string("Configuration error: ") + e.what();
    sched->error(sched, 2, message.c_str());
    errno = EINVAL;
    return -2;
  }

  cs->driver->start();

  return 0;
}


int mesos_sched_unreg(struct mesos_sched* sched)
{
  if (sched == NULL) {
    errno = EINVAL;
    return -1;
  }

  CScheduler* cs = lookupCScheduler(sched);

  if (cs->driver == NULL) {
    errno = EINVAL;
    return -1;
  }

  cs->driver->stop();

  return 0;
}


int mesos_sched_send_message(struct mesos_sched* sched,
                             struct mesos_framework_message* msg)
{
  if (sched == NULL || msg == NULL) {
    errno = EINVAL;
    return -1;
  }

  FrameworkMessage message(string(msg->sid), msg->tid,
                           string((char*) msg->data, msg->data_len));

  CScheduler* cs = lookupCScheduler(sched);

  if (cs->driver == NULL) {
    errno = EINVAL;
    return -1;
  }

  cs->driver->sendFrameworkMessage(message);

  return 0;
}


int mesos_sched_kill_task(struct mesos_sched* sched, task_id tid)
{
  if (sched == NULL) {
    errno = EINVAL;
    return -1;
  }

  CScheduler* cs = lookupCScheduler(sched);

  if (cs->driver == NULL) {
    errno = EINVAL;
    return -1;
  }

  cs->driver->killTask(tid);

  return 0;
}


int mesos_sched_reply_to_offer(struct mesos_sched* sched,
                               offer_id oid,
                               struct mesos_task_desc* tasks,
                               int num_tasks,
                               const char* params)
{
  if (sched == NULL || tasks == NULL || num_tasks < 0) {
    errno = EINVAL;
    return -1;
  }

  vector<TaskDescription> wrapped_tasks(num_tasks);

  for (int i = 0; i < num_tasks; i++) {
    // Convert task's params from key=value pairs to map<string, string>
    map<string, string> params;
    try {
      Params paramsObj(tasks[i].params);
      params = paramsObj.getMap();
    } catch(const ParseException& e) {
      errno = EINVAL;
      return -1;
    }

    // Get task argument as a STL string
    string taskArg((char*) tasks[i].arg, tasks[i].arg_len);

    wrapped_tasks[i] = TaskDescription(tasks[i].tid,
                                       string(tasks[i].sid),
                                       string(tasks[i].name),
                                       params,
                                       taskArg);
  }

  CScheduler* cs = lookupCScheduler(sched);

  if (cs->driver == NULL) {
    errno = EINVAL;
    return -1;
  }

  Params paramsObj(params);
  cs->driver->replyToOffer(oid, wrapped_tasks, paramsObj.getMap());

  return 0;
}


int mesos_sched_revive_offers(struct mesos_sched* sched)
{
  if (sched == NULL) {
    errno = EINVAL;
    return -1;
  }

  CScheduler* cs = lookupCScheduler(sched);

  if (cs->driver == NULL) {
    errno = EINVAL;
    return -1;
  }

  cs->driver->reviveOffers();

  return 0;
}


int mesos_sched_join(struct mesos_sched* sched)
{
  if (sched == NULL) {
    errno = EINVAL;
    return -1;
  }

  CScheduler* cs = lookupCScheduler(sched);

  if (cs->driver == NULL) {
    errno = EINVAL;
    return -1;
  }

  cs->driver->join();

  return 0;
}

} /* extern "C" */
