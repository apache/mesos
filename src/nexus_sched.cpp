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

#include <process.hpp>

#include <boost/bind.hpp>
#include <boost/unordered_map.hpp>

#include "nexus_sched.h"

#include "fatal.hpp"
#include "hash_pid.hpp"
#include "lock.hpp"
#include "messages.hpp"
#include "nexus_local.hpp"
#include "nexus_sched.hpp"
#include "url_prcessor.hpp"
#include "leader_detector.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::map;
using std::string;
using std::vector;

using boost::bind;
using boost::ref;
using boost::unordered_map;

using namespace nexus;
using namespace nexus::internal;


namespace nexus { namespace internal {

/**
 * TODO(benh): Update this comment.
 * Scheduler process, responsible for interacting with the master
 * and responding to Nexus API calls from schedulers. In order to
 * allow a message to be sent back to the master we allow friend
 * functions to invoke 'send'. Therefore, care must be done to insure
 * any synchronization necessary is performed.
 */

class SchedulerProcess : public Tuple<Process>
{
public:
  friend class nexus::NexusSchedulerDriver;

private:
  PID master;
  NexusSchedulerDriver* driver;
  Scheduler* sched;
  FrameworkID fid;
  string frameworkName;
  ExecutorInfo execInfo;
  bool isFT;
  string zkservers;
  LeaderDetector *leaderDetector;

  volatile bool terminate;

  class SchedLeaderListener;
  friend class SchedLeaderListener;

  class SchedLeaderListener : public LeaderListener {
  public:
    // TODO(alig): make thread safe
    SchedLeaderListener(SchedulerProcess *s, PID pp) : parent(s), parentPID(pp) {}
    
    virtual void newLeaderElected(string zkId, string pidStr) {
      if (zkId!="") {
	LOG(INFO) << "Leader listener detected leader at " << pidStr <<" with ephemeral id:"<<zkId;
	
	parent->zkservers = pidStr;

	LOG(INFO) << "Sending message to parent "<<parentPID<<" about new leader";
	parent->send(parentPID, parent->pack<LE_NEWLEADER>(pidStr));

      }
    }

  private:
    SchedulerProcess *parent;
    PID parentPID;
  } schedLeaderListener;


public:
  SchedulerProcess(const PID &_master,
                   NexusSchedulerDriver* _driver,
                   Scheduler* _sched,
                   const string& _frameworkName,
                   const ExecutorInfo& _execInfo,
		   const string& _zkservers="")
    : master(_master),
      driver(_driver),
      sched(_sched),
      fid("-1"),
      terminate(false),
      frameworkName(_frameworkName),
      execInfo(_execInfo),
      leaderDetector(NULL),
      schedLeaderListener(this, getPID())
{
  if (_zkservers!="") {
    pair<UrlProcessor::URLType, string> urlPair = UrlProcessor::process(_zkservers);
    if (urlPair.first == UrlProcessor::ZOO) {
      isFT=true;
      zkservers = urlPair.second;
    } else {
      cerr << "Failed to parse URL for ZooKeeper servers";
      exit(1);
    }
  }
}

protected:
  void operator () ()
  {
    // Get username of current user
    struct passwd* passwd;
    if ((passwd = getpwuid(getuid())) == NULL)
      fatal("failed to get username information");
    string user(passwd->pw_name);

    if (isFT) {
      LOG(INFO) << "Connecting to ZooKeeper at " << zkservers;
      leaderDetector = new LeaderDetector(zkservers, false, "", NULL);
      leaderDetector->setListener(&schedLeaderListener); // use this instead of constructor to avoid race condition

      pair<string,string> zkleader = leaderDetector->getCurrentLeader();
      LOG(INFO) << "Detected leader at " << zkleader.second <<" with ephemeral id:"<<zkleader.first;
      
      istringstream iss(zkleader.second);
      if (!(iss >> master)) {
	cerr << "Failed to resolve master PID " << zkleader.second << endl;
      }    
    }

    link(master);
    send(master, pack<F2M_REGISTER_FRAMEWORK>(frameworkName, user, execInfo));

    while(true) {
      // Rather than send a message to this process when it is time to
      // complete, we set a flag that gets re-read. Sending a message
      // requires some sort of matching or priority reads that
      // libprocess currently doesn't support. Note that this field is
      // only read by this process (after setting it in the
      // destructor), so we don't need to protect it in any way. In
      // fact, using a lock to protect it (or for providing atomicity
      // for cleanup, for example), might lead to deadlock with the
      // client code because we already use a lock in SchedulerDriver. That
      // being said, for now we make terminate 'volatile' to guarantee
      // that each read is getting a fresh copy.
      // TODO(benh): Do a coherent read so as to avoid using 'volatile'.
      if (terminate)
        return;

      switch(receive()) {
      case M2F_REGISTER_REPLY: {
        unpack<M2F_REGISTER_REPLY>(fid);
        invoke(bind(&Scheduler::registered, sched, driver, fid));
        break;
      }

      case M2F_SLOT_OFFER: {
        OfferID oid;
        vector<SlaveOffer> offs;
        unpack<M2F_SLOT_OFFER>(oid, offs);
        invoke(bind(&Scheduler::resourceOffer, sched, driver, oid, ref(offs)));
        break;
      }

      case M2F_RESCIND_OFFER: {
        OfferID oid;
        unpack<M2F_RESCIND_OFFER>(oid);
        invoke(bind(&Scheduler::offerRescinded, sched, driver, oid));
        break;
      }

      case M2F_STATUS_UPDATE: {
        TaskID tid;
        TaskState state;
        string data;
        unpack<M2F_STATUS_UPDATE>(tid, state, data);
        TaskStatus status(tid, state, data);
        invoke(bind(&Scheduler::statusUpdate, sched, driver, ref(status)));
        break;
      }

      case M2F_FRAMEWORK_MESSAGE: {
        FrameworkMessage msg;
        unpack<M2F_FRAMEWORK_MESSAGE>(msg);
        invoke(bind(&Scheduler::frameworkMessage, sched, driver, ref(msg)));
        break;
      }

      case M2F_LOST_SLAVE: {
        SlaveID sid;
        unpack<M2F_LOST_SLAVE>(sid);
        invoke(bind(&Scheduler::slaveLost, sched, driver, sid));
        break;
      }

      case M2F_ERROR: {
        int32_t code;
        string message;
        unpack<M2F_ERROR>(code, message);
        invoke(bind(&Scheduler::error, sched, driver, code, ref(message)));
        break;
      }


      case PROCESS_EXIT: {
        const char* message = "Connection to master failed";
	LOG(INFO) << message;
	//        invoke(bind(&Scheduler::error, sched, driver, -1, message));
        break;
      }

      case LE_NEWLEADER: {
        LOG(INFO) << "Slave got notified of new leader " << from();
	string newLeader;
        unpack<LE_NEWLEADER>(newLeader);
	istringstream iss(newLeader);
	if (!(iss >> master)) {
	  cerr << "Failed to resolve master PID " << newLeader << endl;
	  break;
	}    
	
	LOG(INFO) << "Connecting to Nexus master at " << master;
	link(master);
	send(master, pack<F2M_REGISTER_FRAMEWORK>(frameworkName, user, execInfo));
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

}} /* namespace nexus { namespace internal { */


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
void Scheduler::error(SchedulerDriver* s, int code, const string &message)
{
  cerr << "Nexus error: " << message
       << " (error code: " << code << ")" << endl;
  s->stop();
}


NexusSchedulerDriver::NexusSchedulerDriver(Scheduler* _sched,
                               const string &_master)
  : sched(_sched), master(_master), running(false), process(NULL)
{
  // Create mutex and condition variable
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  pthread_cond_init(&cond, 0);
}


NexusSchedulerDriver::~NexusSchedulerDriver()
{
  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);

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
  Process::wait(process);
  delete process;
}


void NexusSchedulerDriver::start()
{
  Lock lock(&mutex);

  if (running) {
    error(1, "cannot call start - scheduler is already running");
    return;
  }

  PID pid;
  bool passString = false;

  if (master == string("localquiet")) {
    // TODO(benh): Look up resources in environment variables.
    pid = run_nexus(1, 1, 1073741824, true, true);
  } else if (master == string("local")) {
    // TODO(benh): Look up resources in environment variables.
    pid = run_nexus(1, 1, 1073741824, true, false);
  } else
    passString = true;

  const string& frameworkName = sched->getFrameworkName(this);
  const ExecutorInfo& executorInfo = sched->getExecutorInfo(this);

  if (passString) 
    process = new SchedulerProcess(pid, this, sched, frameworkName, executorInfo, master);
  else
    process = new SchedulerProcess(pid, this, sched, frameworkName, executorInfo);
  Process::spawn(process);

  running = true;
}



void NexusSchedulerDriver::stop()
{
  Lock lock(&mutex);

  if (!running) {
    // Don't issue an error (could lead to an infinite loop).
    // TODO(benh): It would be much cleaner to return success or failure!
    return;
  }

  // TODO(benh): Do a Process::post instead?
  process->send(process->master,
                process->pack<F2M_UNREGISTER_FRAMEWORK>(process->fid));

  process->terminate = true;

  running = false;

  pthread_cond_signal(&cond);
}


void NexusSchedulerDriver::join()
{
  Lock lock(&mutex);
  while (running)
    pthread_cond_wait(&cond, &mutex);
}


void NexusSchedulerDriver::run()
{
  start();
  join();
}


void NexusSchedulerDriver::killTask(TaskID tid)
{
  Lock lock(&mutex);

  if (!running) {
    error(1, "cannot call killTask - scheduler is not running");
    return;
  }

  // TODO(benh): Do a Process::post instead?

  process->send(process->master,
                process->pack<F2M_KILL_TASK>(process->fid, tid));
}


void NexusSchedulerDriver::replyToOffer(OfferID offerId,
                                        const vector<TaskDescription> &tasks,
                                        const string_map &params)
{
  Lock lock(&mutex);

  if (!running) {
    error(1, "cannot call replyToOffer - scheduler is not running");
    return;
  }

  // TODO(benh): Do a Process::post instead?

  process->send(process->master,
                process->pack<F2M_SLOT_OFFER_REPLY>(process->fid,
                                                    offerId,
                                                    tasks,
                                                    Params(params)));
}


void NexusSchedulerDriver::reviveOffers()
{
  Lock lock(&mutex);

  if (!running) {
    error(1, "cannot call reviveOffers - scheduler is not running");
    return;
  }

  // TODO(benh): Do a Process::post instead?

  process->send(process->master,
                process->pack<F2M_REVIVE_OFFERS>(process->fid));
}


void NexusSchedulerDriver::sendFrameworkMessage(const FrameworkMessage &message)
{
  Lock lock(&mutex);

  if (!running) {
    error(1, "cannot call sendFrameworkMessage - scheduler is not running");
    return;
  }

  process->send(process->master,
                process->pack<F2M_FRAMEWORK_MESSAGE>(process->fid, message));
}


void NexusSchedulerDriver::sendHints(const string_map& hints)
{
  Lock lock(&mutex);

  if (!running) {
    error(1, "cannot call sendHints - scheduler is not running");
    return;
  }

  // TODO(*): Send the hints; for now, we do nothing
  error(1, "sendHints is not yet implemented");
}


void NexusSchedulerDriver::error(int code, const string& message)
{
  sched->error(this, code, message);
}


/*
 * Implementation of C API. We comprimise the performance of the C API
 * in favor of the performance of the C++ API by having the C API call
 * into the C++ API. Having the C++ API call into the C API is slower
 * because the rest of Nexus is written in C++, which means
 * translations are required when going from C++ to C and then back to
 * C++ when having the C++ API call into the C API.
 */


namespace nexus { namespace internal {

/*
 * We wrap calls from the C API into the C++ API with the following
 * specialized implementation of SchedulerDriver.
 */
class CScheduler : public Scheduler
{
public:
  string frameworkName;
  ExecutorInfo execInfo;
  nexus_sched* sched;
  SchedulerDriver* driver; // Set externally after object is created

  CScheduler(string fwName,
                      string execUri,
                      string execArg,
                      nexus_sched* _sched)
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
    nexus_slot* c_offers = new nexus_slot[offers.size()];
    for (size_t i = 0; i < offers.size(); i++) {
      nexus_slot offer = { offers[i].slaveId.c_str(),
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
    nexus_task_status c_status = { status.taskId,
                                   status.state,
                                   status.data.data(),
                                   status.data.size() };
    sched->status_update(sched, &c_status);
  }

  virtual void frameworkMessage(SchedulerDriver*, FrameworkMessage &message)
  {
    nexus_framework_message c_message = { message.slaveId.c_str(),
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
 * We record the mapping from nexus_sched to CScheduler. It would
 * be preferable to somehow link the two without requiring any extra data
 * structures, but without adding some random field to nexus_sched (or
 * doing some object prefix approach), this is the best we got.
 *
 * TODO(benh): Make access to these thread safe once we have some
 * locking mechanism that abstracts away if we are using pthreads or
 * libprocess.
 */
unordered_map<nexus_sched* , CScheduler*> c_schedulers;


CScheduler* lookupCScheduler(nexus_sched* sched)
{
  assert(sched != NULL);

  // TODO(benh): Protect 'c_schedulers' (see above).
  unordered_map<nexus_sched*, CScheduler*>::iterator it =
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


void removeCScheduler(nexus_sched* sched)
{
  // TODO(benh): Protect 'c_schedulers' (see above).
  unordered_map<nexus_sched*, CScheduler*>::iterator it =
    c_schedulers.find(sched);

  CScheduler* cs = it == c_schedulers.end() ? NULL : it->second;

  if (cs != NULL) {
    c_schedulers.erase(sched);
    if (cs->driver != NULL)
      delete cs->driver;
    delete cs;
  }
}


}} /* namespace nexus { namespace internal { */


extern "C" {

// TODO(*): For safety, it would be better if we allocate the
// nexus_sched objects (that way we won't ever call into them and
// segfault in our code), but that's not the C way, probably because
// it means nexus_sched objects can't get put on the stack.

int nexus_sched_init(struct nexus_sched* sched)
{
  if (sched == NULL) {
    errno = EINVAL;
    return -1;
  }

  return 0;
}


int nexus_sched_destroy(struct nexus_sched* sched)
{
  if (sched == NULL) {
    errno = EINVAL;
    return -1;
  }

  removeCScheduler(sched);

  return 0;
}


int nexus_sched_reg(struct nexus_sched* sched, const char* master)
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

  cs->driver = new NexusSchedulerDriver(cs, master);
  cs->driver->start();

  return 0;
}


int nexus_sched_unreg(struct nexus_sched* sched)
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


int nexus_sched_send_message(struct nexus_sched* sched,
                             struct nexus_framework_message* msg)
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


int nexus_sched_kill_task(struct nexus_sched* sched, task_id tid)
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


int nexus_sched_reply_to_offer(struct nexus_sched* sched,
                               offer_id oid,
                               struct nexus_task_desc* tasks,
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


int nexus_sched_revive_offers(struct nexus_sched* sched)
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


int nexus_sched_join(struct nexus_sched* sched)
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
