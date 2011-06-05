#include <getopt.h>

#include <fstream>
#include <algorithm>
#include "slave.hpp"
#include "slave_webui.hpp"
#include "isolation_module_factory.hpp"

using std::list;
using std::make_pair;
using std::ostringstream;
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

namespace {

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


Slave::Slave(const string &_master, Resources _resources, bool _local)
  : leaderDetector(NULL), 
    resources(_resources), local(_local), id("-1"),
    isolationType("process"), isolationModule(NULL), slaveLeaderListener(this, getPID())
{
  pair<Slave::URLType, string> urlPair = processUrl(_master);
  if (urlPair.first == Slave::ZOOURL) {
    isFT=true;
    zkserver = urlPair.second;
  } else if (urlPair.first == Slave::NEXUSURL) {
    isFT=false;
    istringstream iss(urlPair.second);
    if (!(iss >> master)) {
      cerr << "Failed to resolve master PID " << urlPair.second << endl;
      exit(1);
    }
  } else {
    cerr << "Failed to parse URL for Nexus master or ZooKeeper servers ";
    exit(1);
  }
}


Slave::Slave(const string &_master, Resources _resources, bool _local,
	     const string& _isolationType)
  : leaderDetector(NULL), 
    resources(_resources), local(_local), id("-1"),
    isolationType(_isolationType), isolationModule(NULL), slaveLeaderListener(this, getPID())
{
  pair<Slave::URLType, string> urlPair = processUrl(_master);
  if (urlPair.first == Slave::ZOOURL) {
    isFT=true;
    zkserver = urlPair.second;
  } else if (urlPair.first == Slave::NEXUSURL) {
    isFT=false;
    istringstream iss(urlPair.second);
    if (!(iss >> master)) {
      LOG(ERROR) << "Failed to resolve master PID " << urlPair.second;
      exit(1);
    }
  } else {
    LOG(ERROR) << "Failed to parse URL for Nexus master or ZooKeeper servers ";
    exit(1);
  }
}

//  enum URLType {ZOOURL, NEXUSURL};
pair<Slave::URLType, string> Slave::processUrl(const string &_url) {
  
  string url = _url;

  transform(url.begin(), url.end(), url.begin(), (int (*)(int))toupper);

  if (url.find("ZOO://")==0) {

    return pair<Slave::URLType, string>(Slave::ZOOURL, url.substr(6,1024));

  } else if (url.find("ZOOFILE://")==0) {
    string zfname = _url.substr(10,1024);
    string zoos="";
    
    LOG(INFO)<<"Opening ZooFile: "<<zfname;
    ifstream zoofile(zfname.c_str());
    if (!zoofile) 
      LOG(ERROR)<<"ZooFile "<<zfname<<" could not be opened";

    while(!zoofile.eof()) {
      string line;
      getline(zoofile, line);
      if (line=="")
        continue;
      if (zoos!="")
        zoos+=',';
      zoos+=line;
    }

    remove_if(zoos.begin(),zoos.end(), (int (*)(int)) isspace);
    zoofile.close();

    return pair<Slave::URLType, string>(Slave::ZOOURL, zoos);

  } else if (url.find("NEXUS://")==0) {
    return pair<Slave::URLType, string>(Slave::NEXUSURL, url.substr(8,1024));
  } else
    LOG(ERROR)<<"Could not parse master/zoo URL";
    return pair<Slave::URLType, string>(Slave::NONEURL, "");
}

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
    foreachpair(_, TaskInfo *t, f->tasks) {
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

  if (isFT) {
    LOG(INFO) << "Connecting to ZooKeeper at " << zkserver;
    leaderDetector = new LeaderDetector(zkserver, false, "", NULL);
    leaderDetector->setListener(&slaveLeaderListener); // use this instead of constructor to avoid race condition

    pair<string,string> zkleader = leaderDetector->getCurrentLeader();
    LOG(INFO) << "Detected leader at " << zkleader.second <<" with ephemeral id:"<<zkleader.first;

    istringstream iss(zkleader.second);
    if (!(iss >> master)) {
      cerr << "Failed to resolve master PID " << zkleader.second << endl;
    }    
  }

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

  LOG(INFO) << "Connecting to Nexus master at " << master;
  link(master);
  send(master, pack<S2M_REGISTER_SLAVE>(hostname, publicDns, resources));
  
  FrameworkID fid;
  TaskID tid;
  TaskState taskState;
  Params params;
  FrameworkMessage message;
  string data;

  while (true) {
    switch (receive()) {
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
        TaskInfo *task = framework->addTask(tid, taskName, res);
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
	  // if (isolationModule != NULL)
	  //   delete isolationModule;
	  // // TODO: Shut down executors?
	  // return;
	}

        foreachpair (_, Executor *ex, executors) {
          if (from() == ex->pid) {
            LOG(INFO) << "Executor for framework " << ex->frameworkId
                      << " disconnected";
            send(master, pack<S2M_LOST_EXECUTOR>(id, ex->frameworkId, -1));
            removeExecutor(ex->frameworkId, true);
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

      case LE2S_NEWLEADER: {
        LOG(INFO) << "Slave got notified of new leader " << from();
	string newLeader;
        unpack<LE2S_NEWLEADER>(newLeader);
	istringstream iss(newLeader);
	if (!(iss >> master)) {
	  cerr << "Failed to resolve master PID " << newLeader << endl;
	  break;
	}    
	
	LOG(INFO) << "Connecting to Nexus master at " << master;
	link(master);

	// reconstruct resourcesInUse for the master
	// alig: do I need to include queuedTasks in this number? Don't think so.
	Resources resourcesInUse; 
	vector<TaskInfo> taskVec;

	foreachpair(_, Framework *framework, frameworks) {
	  foreachpair(_, TaskInfo *task, framework->tasks) {
	    resourcesInUse += task->resources;
	    TaskInfo ti = *task;
	    ti.slaveId = id;
	    taskVec.push_back(ti);
	  }
	}
	//alibandali

	send(master, pack<S2M_REREGISTER_SLAVE>(hostname, publicDns, resources, taskVec));
	
	break;
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
