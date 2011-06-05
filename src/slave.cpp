#include <fstream>
#include <algorithm>

#include "slave.hpp"
#include "slave_webui.hpp"

// There's no gethostbyname2 on Solaris, so fake it by calling gethostbyname
#ifdef __sun__
#define gethostbyname2(name, _) gethostbyname(name)
#endif

using std::list;
using std::make_pair;
using std::ostringstream;
using std::istringstream;
using std::pair;
using std::queue;
using std::string;
using std::vector;

using boost::lexical_cast;
using boost::unordered_map;
using boost::unordered_set;

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;


namespace {

// Periodically sends heartbeats to the master
class Heart : public Tuple<Process>
{
private:
  PID master;
  PID slave;
  SlaveID sid;
  double interval;

protected:
  void operator () ()
  {
    link(slave);
    do {
      switch (receive(interval)) {
      case PROCESS_TIMEOUT:
	send(master, pack<SH2M_HEARTBEAT>(sid));
	break;
      case PROCESS_EXIT:
	return;
      }
    } while (true);
  }

public:
  Heart(const PID &_master, const PID &_slave, SlaveID _sid, double _interval)
    : master(_master), slave(_slave), sid(_sid), interval(_interval) {}
};


// Default values for CPU cores and memory to include in configuration
const int32_t DEFAULT_CPUS = 1;
const int64_t DEFAULT_MEM = 1 * Gigabyte;


} /* namespace */


Slave::Slave(Resources _resources, bool _local,
             IsolationModule *_isolationModule)
  : id(""), resources(_resources), local(_local),
    isolationModule(_isolationModule) {}


Slave::Slave(const Params& _conf, bool _local, IsolationModule *_module)
  : id(""), conf(_conf), local(_local), isolationModule(_module)
{
  resources = Resources(conf.get<int32_t>("cpus", DEFAULT_CPUS),
                        conf.get<int64_t>("mem", DEFAULT_MEM));
}


void Slave::registerOptions(Configurator* conf)
{
  conf->addOption<int32_t>("cpus", 'c', "CPU cores for use by tasks",
                           DEFAULT_CPUS);
  conf->addOption<int64_t>("mem", 'm', "Memory for use by tasks, in bytes\n",
                           DEFAULT_MEM);
  conf->addOption<string>("work_dir",
                          "Where to place framework work directories\n"
                          "(default: MESOS_HOME/work)");
}


Slave::~Slave()
{
  // TODO(benh): Shut down and free executors?
}


state::SlaveState *Slave::getState()
{
  std::ostringstream my_pid;
  my_pid << self();
  std::ostringstream master_pid;
  master_pid << master;
  state::SlaveState *state =
    new state::SlaveState(BUILD_DATE, BUILD_USER, id, resources.cpus, 
        resources.mem, my_pid.str(), master_pid.str());

  foreachpair(_, Framework *f, frameworks) {
    state::Framework *framework = new state::Framework(f->id, f->name, 
        f->executorInfo.uri, f->executorStatus, f->resources.cpus,
        f->resources.mem);
    state->frameworks.push_back(framework);
    foreachpair(_, Task *t, f->tasks) {
      state::Task *task = new state::Task(t->id, t->name, t->state,
          t->resources.cpus, t->resources.mem);
      framework->tasks.push_back(task);
    }
  }

  return state;
}


void Slave::operator () ()
{
  LOG(INFO) << "Slave started at " << self();

  // Get our hostname
  char buf[256];
  gethostname(buf, sizeof(buf));
  hostent *he = gethostbyname2(buf, AF_INET);
  const char *hostname = he->h_name;

  // Get our public DNS name. Normally this is our hostname, but on EC2
  // we look for the MESOS_PUBLIC_DNS environment variable. This allows
  // the master to display our public name in its web UI.
  const char *publicDns = getenv("MESOS_PUBLIC_DNS");
  if (!publicDns) {
    publicDns = hostname;
  }

  // Initialize isolation module.
  isolationModule->initialize(this);

  while (true) {
    switch (receive()) {
      case NEW_MASTER_DETECTED: {
	string masterSeq;
	PID masterPid;
	unpack<NEW_MASTER_DETECTED>(masterSeq, masterPid);

	LOG(INFO) << "New master at " << masterPid << " with ID:" << masterSeq;

        redirect(master, masterPid);
	master = masterPid;
	link(master);

	if (id.empty()) {
	  // Slave started before master.
	  send(master, pack<S2M_REGISTER_SLAVE>(hostname, publicDns, resources));
	} else {
	  // Reconnecting, so reconstruct resourcesInUse for the master.
	  Resources resourcesInUse; 
	  vector<Task> taskVec;

	  foreachpair(_, Framework *framework, frameworks) {
	    foreachpair(_, Task *task, framework->tasks) {
	      resourcesInUse += task->resources;
	      Task ti = *task;
	      ti.slaveId = id;
	      taskVec.push_back(ti);
	    }
	  }

	  send(master, pack<S2M_REREGISTER_SLAVE>(id, hostname, publicDns, resources, taskVec));
	}
	break;
      }
	
      case NO_MASTER_DETECTED: {
	LOG(INFO) << "Lost master(s) ... waiting";
	break;
      }

      case M2S_REGISTER_REPLY: {
	double interval = 0;
        unpack<M2S_REGISTER_REPLY>(this->id, interval);
        LOG(INFO) << "Registered with master; given slave ID " << this->id;
        link(spawn(new Heart(master, self(), this->id, interval)));
        break;
      }
      
      case M2S_REREGISTER_REPLY: {
        FrameworkID tmpfid;
	double interval = 0;
        unpack<M2S_REGISTER_REPLY>(tmpfid, interval);
        LOG(INFO) << "RE-registered with master; given slave ID " << tmpfid << " had "<< this->id;
        if (this->id == "")
          this->id = tmpfid;
        link(spawn(new Heart(master, self(), this->id, interval)));
        break;
      }
      
      case M2S_RUN_TASK: {
	FrameworkID fid;
        TaskID tid;
        string fwName, user, taskName, taskArg, fwPidStr;
        ExecutorInfo execInfo;
        Params params;
        unpack<M2S_RUN_TASK>(fid, tid, fwName, user, execInfo,
                             taskName, taskArg, params, fwPidStr);
        LOG(INFO) << "Got assigned task " << fid << ":" << tid;
        Resources res;
        res.cpus = params.getInt32("cpus", -1);
        res.mem = params.getInt64("mem", -1);
        Framework *framework = getFramework(fid);
        if (framework == NULL) {
          // Framework not yet created on this node - create it
          PID fwPid(fwPidStr);
          if (!fwPid) {
            LOG(ERROR) << "Couldn't create PID out of framework PID string";
          }
          framework = new Framework(fid, fwName, user, execInfo, fwPid);
          frameworks[fid] = framework;
          isolationModule->frameworkAdded(framework);
          isolationModule->startExecutor(framework);
        }
        Task *task = framework->addTask(tid, taskName, res);
        Executor *executor = getExecutor(fid);
        if (executor) {
          send(executor->pid,
               pack<S2E_RUN_TASK>(tid, taskName, taskArg, params));
          isolationModule->resourcesChanged(framework);
        } else {
          // Executor not yet registered; queue task for when it starts up
          TaskDescription *td = new TaskDescription(
              tid, taskName, taskArg, params.str());
          framework->queuedTasks.push_back(td);
        }
        break;
      }

      case M2S_KILL_TASK: {
        FrameworkID fid;
        TaskID tid;
        unpack<M2S_KILL_TASK>(fid, tid);
        LOG(INFO) << "Killing task " << fid << ":" << tid;
        if (Executor *ex = getExecutor(fid)) {
          send(ex->pid, pack<S2E_KILL_TASK>(tid));
        }
        if (Framework *fw = getFramework(fid)) {
          fw->removeTask(tid);
          isolationModule->resourcesChanged(fw);
        }
        // Report to master that the task is killed
        send(master, pack<S2M_STATUS_UPDATE>(id, fid, tid, TASK_KILLED, ""));
        break;
      }

      case M2S_FRAMEWORK_MESSAGE: {
        FrameworkID fid;
        FrameworkMessage message;
        unpack<M2S_FRAMEWORK_MESSAGE>(fid, message);
        if (Executor *ex = getExecutor(fid)) {
          send(ex->pid, pack<S2E_FRAMEWORK_MESSAGE>(message));
        }
        // TODO(*): If executor is not started, queue framework message?
        // (It's probably okay to just drop it since frameworks can have
        // the executor send a message to the master to say when it's ready.)
        break;
      }

      case M2S_KILL_FRAMEWORK: {
        FrameworkID fid;
        unpack<M2S_KILL_FRAMEWORK>(fid);
        LOG(INFO) << "Asked to kill framework " << fid;
        Framework *fw = getFramework(fid);
        if (fw != NULL)
          killFramework(fw);
        break;
      }

      case E2S_REGISTER_EXECUTOR: {
        FrameworkID fid;
        unpack<E2S_REGISTER_EXECUTOR>(fid);
        LOG(INFO) << "Got executor registration for framework " << fid;
        if (Framework *fw = getFramework(fid)) {
          if (getExecutor(fid) != 0) {
            LOG(ERROR) << "Executor for framework " << fid
                       << "already exists";
            send(from(), pack<S2E_KILL_EXECUTOR>());
            break;
          }
          Executor *executor = new Executor(fid, from());
          executors[fid] = executor;
          link(from());
          // Now that the executor is up, set its resource limits
          isolationModule->resourcesChanged(fw);
          // Tell executor that it's registered and give it its queued tasks
          send(from(), pack<S2E_REGISTER_REPLY>(this->id,
                                                fw->name,
                                                fw->executorInfo.initArg));
          sendQueuedTasks(fw);
        } else {
          // Framework is gone; tell the executor to exit
          send(from(), pack<S2E_KILL_EXECUTOR>());
        }
        break;
      }

      case E2S_STATUS_UPDATE: {
        FrameworkID fid;
        TaskID tid;
        TaskState taskState;
        string data;
        unpack<E2S_STATUS_UPDATE>(fid, tid, taskState, data);
        LOG(INFO) << "Got status update for task " << fid << ":" << tid;
        if (taskState == TASK_FINISHED || taskState == TASK_FAILED ||
            taskState == TASK_KILLED || taskState == TASK_LOST) {
          LOG(INFO) << "Task " << fid << ":" << tid << " done";
          if (Framework *fw = getFramework(fid)) {
            fw->removeTask(tid);
            isolationModule->resourcesChanged(fw);
          }
        }

        // Pass on the update to the master.
	const string &msg =
	  tupleToString(pack<S2M_FT_STATUS_UPDATE>(id, fid, tid, taskState,
						   data));

        // Reliably send message and save sequence number for canceling later.
	int seq = rsend(master, S2M_FT_STATUS_UPDATE, msg.data(), msg.size());
        seqs[fid].insert(seq);
        break;
      }

      case E2S_FRAMEWORK_MESSAGE: {
        FrameworkID fid;
        FrameworkMessage message;
        unpack<E2S_FRAMEWORK_MESSAGE>(fid, message);
        // Set slave ID in case framework omitted it
        message.slaveId = this->id;
        send(getFramework(fid)->fwPid, pack<M2F_FRAMEWORK_MESSAGE>(message));
        break;
      }

      case S2S_GET_STATE: {
        send(from(), pack<S2S_GET_STATE_REPLY>((int64_t) getState()));
        break;
      }

      case PROCESS_EXIT: {
        LOG(INFO) << "Process exited: " << from();

        if (from() == master) {
	  LOG(WARNING) << "Master disconnected! "
		       << "Waiting for a new master to be elected.";
	  // TODO(benh): After so long waiting for a master, commit suicide.
	} else {
	  // Check if an executor has exited.
	  foreachpair (_, Executor *ex, executors) {
	    if (from() == ex->pid) {
	      LOG(INFO) << "Executor for framework " << ex->frameworkId
			<< " disconnected";
	      Framework *framework = getFramework(ex->frameworkId);
	      if (framework != NULL) {
		send(master, pack<S2M_LOST_EXECUTOR>(id, ex->frameworkId, -1));
		killFramework(framework);
	      }
	      break;
	    }
	  }
	}

        break;
      }

      case M2S_SHUTDOWN: {
        LOG(INFO) << "Asked to shut down by master: " << from();
        unordered_map<FrameworkID, Framework*> frameworksCopy = frameworks;
        foreachpair (_, Framework *framework, frameworksCopy) {
          killFramework(framework);
        }
        return;
      }

      case S2S_SHUTDOWN: {
        LOG(INFO) << "Asked to shut down by " << from();
        unordered_map<FrameworkID, Framework*> frameworksCopy = frameworks;
        foreachpair (_, Framework *framework, frameworksCopy) {
          killFramework(framework);
        }
        return;
      }

      default: {
        LOG(ERROR) << "Received unknown message ID " << msgid()
                   << " from " << from();
        break;
      }
    }
  }
}


Framework * Slave::getFramework(FrameworkID frameworkId)
{
  FrameworkMap::iterator it = frameworks.find(frameworkId);
  if (it == frameworks.end()) return NULL;
  return it->second;
}


Executor * Slave::getExecutor(FrameworkID frameworkId)
{
  ExecutorMap::iterator it = executors.find(frameworkId);
  if (it == executors.end()) return NULL;
  return it->second;
}


// Send any tasks queued up for the given framework to its executor
// (needed if we received tasks while the executor was starting up)
void Slave::sendQueuedTasks(Framework *framework)
{
  LOG(INFO) << "Flushing queued tasks for framework " << framework->id;
  Executor *executor = getExecutor(framework->id);
  if (!executor) return;
  foreach(TaskDescription *td, framework->queuedTasks) {
    send(executor->pid,
        pack<S2E_RUN_TASK>(td->tid, td->name, td->args, td->params));
    delete td;
  }
  framework->queuedTasks.clear();
}


// Remove a framework's Executor. If killProcess is true, also
// ask the isolation module to kill it.
void Slave::removeExecutor(FrameworkID frameworkId, bool killProcess)
{
  if (Framework *framework = getFramework(frameworkId)) {
    LOG(INFO) << "Cleaning up executor for framework " << frameworkId;
    Executor *ex = getExecutor(frameworkId);
    if (ex != NULL) {
      delete ex;
      executors.erase(frameworkId);
    }
    if (killProcess) {
      LOG(INFO) << "Killing executor for framework " << frameworkId;
      isolationModule->killExecutor(framework);
    }
  }
}


// Kill a framework (including its executor)
void Slave::killFramework(Framework *fw)
{
  LOG(INFO) << "Cleaning up framework " << fw->id;

  // Cancel sending any reliable messages for this framework.
  foreach (int seq, seqs[fw->id])
    cancel(seq);

  seqs.erase(fw->id);

  // Remove its allocated resources.
  fw->resources = Resources();

  // If an executor is running, tell it to exit and kill it
  if (Executor *ex = getExecutor(fw->id)) {
    send(ex->pid, pack<S2E_KILL_EXECUTOR>());
    // TODO(benh): There really isn't much time between when an
    // executor gets a S2E_KILL_EXECUTOR message and the isolation
    // module goes and kills it. We should really think about making
    // the semantics of this better.
    removeExecutor(fw->id, true);
  }
  frameworks.erase(fw->id);
  isolationModule->frameworkRemoved(fw);
  delete fw;
}


// Called by isolation module when an executor process exits
// TODO(benh): Make this callback be a message so that we can avoid
// race conditions.
void Slave::executorExited(FrameworkID fid, int status)
{
  if (Framework *f = getFramework(fid)) {
    LOG(INFO) << "Executor for framework " << fid << " exited "
              << "with status " << status;
    send(master, pack<S2M_LOST_EXECUTOR>(id, fid, status));
    removeExecutor(fid, false);
  }
};


string Slave::getWorkDirectory(FrameworkID fid) {
  string workDir;
  if (conf.contains("work_dir")) {
    workDir = conf["work_dir"];
  } else if (conf.contains("home")) {
    workDir = conf["home"] + "/work";
  } else {
    workDir = "work";
  }
  ostringstream fwDir;
  fwDir << workDir << "/slave-" << id << "/fw-" << fid;
  return fwDir.str();
}


const Params& Slave::getConf()
{
  return conf;
}
