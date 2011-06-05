#include "config.hpp" // Need to define first to get USING_ZOOKEEPER

#include <getopt.h>

#ifdef USING_ZOOKEEPER
#include <zookeeper.hpp>
#endif

#include "isolation_module_factory.hpp"
#include "slave.hpp"
#include "slave_webui.hpp"

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

using namespace nexus;
using namespace nexus::internal;
using namespace nexus::internal::slave;


// There's no gethostbyname2 on Solaris, so fake it by calling gethostbyname
#ifdef __sun__
#define gethostbyname2(name, _) gethostbyname(name)
#endif


/* List of ZooKeeper host:port pairs (from slave_main.cpp/local.cpp). */
extern string zookeeper;

namespace {

#ifdef USING_ZOOKEEPER
class SlaveWatcher : public Watcher
{
private:
  Slave *slave;

public:
  void process(ZooKeeper *zk, int type, int state, const string &path)
  {
    if ((state == ZOO_CONNECTED_STATE) &&
	((type == ZOO_SESSION_EVENT) || (type == ZOO_CREATED_EVENT))) {
      // Lookup master PID.
      string znode = "/home/nexus/master";
      int ret;

      // Check if znode exists, if not, just return and wait.
      ret = zk->exists(znode, true, NULL);

      if (ret == ZNONODE)
	return;

      if (ret != ZOK)
	fatal("failed to get %s! (%s)", znode.c_str(), zerror(ret));

      string result;
      ret = zk->get(znode, false, &result, NULL);
    
      if (ret != ZOK)
	fatal("failed to get %s! (%s)", znode.c_str(), zerror(ret));

      PID master;

      istringstream iss(result);
      if (!(iss >> master))
	fatal("bad data at %s!", znode.c_str());

      // TODO(benh): HACK! Don't just set field in slave instance!
      slave->master = master;

      Process::post(slave->getPID(), S2S_GOT_MASTER);
    } else {
      fatal("unhandled ZooKeeper event!");
    }
  }

  SlaveWatcher(Slave *_slave) : slave(_slave) {}
};
#endif

// Periodically sends heartbeats to the master
class Heart : public Tuple<Process>
{
private:
  PID master;
  PID slave;
  SlaveID sid;

protected:
  void operator () ()
  {
    link(slave);
    do {
      switch (receive(5)) {
      case PROCESS_TIMEOUT:
	send(master, pack<SH2M_HEARTBEAT>(sid));
	break;
      case PROCESS_EXIT:
	return;
      }
    } while (true);
  }

public:
  Heart(const PID &_master, const PID &_slave, SlaveID _sid)
    : master(_master), slave(_slave), sid(_sid) {}
};

} /* namespace */


Slave::Slave(const PID &_master, Resources _resources, bool _local)
  : master(_master), resources(_resources), local(_local), id(-1),
    isolationType("process"), isolationModule(NULL)
{}


Slave::Slave(const PID &_master, Resources _resources, bool _local,
    const string& _isolationType)
  : master(_master), resources(_resources), local(_local), id(-1),
    isolationType(_isolationType), isolationModule(NULL)
{}


Slave::~Slave() {}


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
  // we look for the NEXUS_PUBLIC_DNS environment variable. This allows
  // the master to display our public name in its web UI.
  const char *publicDns = getenv("NEXUS_PUBLIC_DNS");
  if (!publicDns) {
    publicDns = hostname;
  }

#ifdef USING_ZOOKEEPER
  ZooKeeper *zk;
  if (!zookeeper.empty())
    zk = new ZooKeeper(zookeeper, 10000, new SlaveWatcher(this));
#else
  send(self(), pack<S2S_GOT_MASTER>());
#endif
  
  FrameworkID fid;
  TaskID tid;
  TaskState taskState;
  Params params;
  FrameworkMessage message;
  string data;

  while (true) {
    switch (receive()) {
      case S2S_GOT_MASTER: {
	LOG(INFO) << "Connecting to Nexus master at " << master;
	link(master);
	send(master, pack<S2M_REGISTER_SLAVE>(hostname, publicDns, resources));
	break;
      }

      case M2S_REGISTER_REPLY: {
        unpack<M2S_REGISTER_REPLY>(this->id);
        LOG(INFO) << "Registered with master; given slave ID " << this->id;
        link(spawn(new Heart(master, this->getPID(), this->id)));
        isolationModule = createIsolationModule();
        if (!isolationModule)
          LOG(FATAL) << "Unrecognized isolation type: " << isolationType;
        break;
      }
      
      case M2S_RUN_TASK: {
        string fwName, user, taskName, taskArg;
        ExecutorInfo execInfo;
        unpack<M2S_RUN_TASK>(fid, tid, fwName, user, execInfo,
            taskName, taskArg, params);
        LOG(INFO) << "Got assigned task " << fid << ":" << tid;
        Resources res;
        res.cpus = params.getInt32("cpus", -1);
        res.mem = params.getInt64("mem", -1);
        Framework *framework = getFramework(fid);
        if (framework == NULL) {
          // Framework not yet created on this node - create it
          framework = new Framework(fid, fwName, user, execInfo);
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
        unpack<M2S_KILL_TASK>(fid, tid);
        LOG(INFO) << "Killing task " << fid << ":" << tid;
        if (Framework *fw = getFramework(fid)) {
          fw->removeTask(tid);
          isolationModule->resourcesChanged(fw);
        }
        if (Executor *ex = getExecutor(fid)) {
          send(ex->pid, pack<S2E_KILL_TASK>(tid));
        }
        // Report to master that the task is killed
        send(master, pack<S2M_STATUS_UPDATE>(id, fid, tid, TASK_KILLED, ""));
        break;
      }

      case M2S_FRAMEWORK_MESSAGE: {
        unpack<M2S_FRAMEWORK_MESSAGE>(fid, message);
        if (Executor *ex = getExecutor(fid)) {
          send(ex->pid, pack<S2E_FRAMEWORK_MESSAGE>(message));
        }
        // TODO(matei): If executor is not started, queue framework message?
        // (It's probably okay to just drop it since frameworks can have
        // the executor send a message to the master to say when it's ready.)
        break;
      }

      case M2S_KILL_FRAMEWORK: {
        unpack<M2S_KILL_FRAMEWORK>(fid);
        LOG(INFO) << "Asked to kill framework " << fid;
        Framework *fw = getFramework(fid);
        if (fw != NULL)
          killFramework(fw);
        break;
      }

      case E2S_REGISTER_EXECUTOR: {
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
        // Pass on the update to the master
        send(master, pack<S2M_STATUS_UPDATE>(id, fid, tid, taskState, data));
        break;
      }

      case E2S_FRAMEWORK_MESSAGE: {
        unpack<E2S_FRAMEWORK_MESSAGE>(fid, message);
        // Set slave ID in case framework omitted it
        message.slaveId = this->id;
        send(master, pack<S2M_FRAMEWORK_MESSAGE>(id, fid, message));
        break;
      }

      case S2S_GET_STATE: {
        send(from(), pack<S2S_GET_STATE_REPLY>((int64_t) getState()));
        break;
      }

      case PROCESS_EXIT: {
        LOG(INFO) << "Process exited: " << from();

        if (from() == master) {
	  // TODO: Fault tolerance!
          LOG(ERROR) << "Master disconnected! Committing suicide ...";
	  // TODO(matei): Add support for factory style destroy of objects!
	  if (isolationModule != NULL)
	    delete isolationModule;
	  // TODO: Shut down executors?
	  return;
	}

        foreachpair (_, Executor *ex, executors) {
          if (from() == ex->pid) {
            LOG(INFO) << "Executor for framework " << ex->frameworkId
                      << " disconnected";
	    Framework *framework = getFramework(fid);
	    if (framework != NULL) {
	      send(master, pack<S2M_LOST_EXECUTOR>(id, ex->frameworkId, -1));
	      killFramework(framework);
	    }
            break;
          }
        }

        break;
      }

      case M2S_SHUTDOWN: {
        LOG(INFO) << "Asked to shut down by master: " << from();
	// TODO(matei): Add support for factory style destroy of objects!
	if (isolationModule != NULL)
	  delete isolationModule;
        // TODO: Shut down executors?
        return;
      }

      case S2S_SHUTDOWN: {
        LOG(INFO) << "Asked to shut down by " << from();
	// TODO(matei): Add support for factory style destroy of objects!
	if (isolationModule != NULL)
	  delete isolationModule;
        // TODO: Shut down executors?
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


IsolationModule * Slave::createIsolationModule()
{
  LOG(INFO) << "Creating \"" << isolationType << "\" isolation module";
  return IsolationModuleFactory::instantiate(isolationType, this);
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
  fw->resources = Resources();
  // If an executor is running, tell it to exit and kill it
  if (Executor *ex = getExecutor(fw->id)) {
    send(ex->pid, pack<S2E_KILL_EXECUTOR>());
    removeExecutor(fw->id, true);
  }
  frameworks.erase(fw->id);
  isolationModule->frameworkRemoved(fw);
  delete fw;
}


// Called by isolation module when an executor process exits
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
  ostringstream workDir;
  workDir << "work/slave-" << id << "/framework-" << fid;
  return workDir.str();
}
