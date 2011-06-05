#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>

#include <google/protobuf/descriptor.h>

#include "slave.hpp"
#include "webui.hpp"

// There's no gethostbyname2 on Solaris, so fake it by calling gethostbyname
#ifdef __sun__
#define gethostbyname2(name, _) gethostbyname(name)
#endif

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using boost::unordered_map;
using boost::unordered_set;

using process::Promise;
using process::UPID;

using std::list;
using std::make_pair;
using std::ostringstream;
using std::pair;
using std::queue;
using std::string;
using std::vector;


Slave::Slave(const Resources& _resources, bool _local,
             IsolationModule *_isolationModule)
  : resources(_resources), local(_local),
    isolationModule(_isolationModule)
{
  initialize();
}


Slave::Slave(const Configuration& _conf, bool _local,
             IsolationModule* _isolationModule)
  : conf(_conf), local(_local),
    isolationModule(_isolationModule)
{
  resources =
    Resources::parse(conf.get<string>("resources", "cpus:1;mem:1024"));

  initialize();
}


void Slave::registerOptions(Configurator* configurator)
{
  // TODO(benh): Is there a way to specify units for the resources?
  configurator->addOption<string>("resources",
                                  "Total consumable resources per slave\n");
//   configurator->addOption<string>("attributes",
//                                   "Attributes of machine\n");
  configurator->addOption<string>("work_dir",
                                  "Where to place framework work directories\n"
                                  "(default: MESOS_HOME/work)");
  configurator->addOption<string>("hadoop_home",
                                  "Where to find Hadoop installed (for\n"
                                  "fetching framework executors from HDFS)\n"
                                  "(default: look for HADOOP_HOME in\n"
                                  "environment or find hadoop on PATH)");
  configurator->addOption<bool>("switch_user", 
                                "Whether to run tasks as the user who\n"
                                "submitted them rather than the user running\n"
                                "the slave (requires setuid permission)",
                                true);
  configurator->addOption<string>("frameworks_home",
                                  "Directory prepended to relative executor\n"
                                  "paths (default: MESOS_HOME/frameworks)");
}


Slave::~Slave()
{
  // TODO(benh): Shut down and free executors?
}


Promise<state::SlaveState*> Slave::getState()
{
  Resources resources(resources);
  Resource::Scalar cpus;
  Resource::Scalar mem;
  cpus.set_value(-1);
  mem.set_value(-1);
  cpus = resources.getScalar("cpus", cpus);
  mem = resources.getScalar("mem", mem);

  state::SlaveState* state =
    new state::SlaveState(build::DATE, build::USER, slaveId.value(),
                          cpus.value(), mem.value(), self(), master);

  foreachpair (_, Framework* f, frameworks) {
    foreachpair (_, Executor* e, f->executors) {
      Resources resources(e->resources);
      Resource::Scalar cpus;
      Resource::Scalar mem;
      cpus.set_value(-1);
      mem.set_value(-1);
      cpus = resources.getScalar("cpus", cpus);
      mem = resources.getScalar("mem", mem);

      // TOOD(benh): For now, we will add a state::Framework object
      // for each executor that the framework has. Therefore, we tweak
      // the framework ID to also include the associated executor ID
      // to differentiate them. This is so we don't have to make very
      // many changes to the webui right now. Note that this ID
      // construction must be identical to what we do for directory
      // suffix returned from Slave::getUniqueWorkDirectory.

      string id = f->frameworkId.value() + "-" + e->info.executor_id().value();

      state::Framework* framework =
        new state::Framework(id, f->info.name(),
                             e->info.uri(), e->executorStatus,
                             cpus.value(), mem.value());

      state->frameworks.push_back(framework);

      foreachpair (_, Task* t, e->tasks) {
        Resources resources(t->resources());
        Resource::Scalar cpus;
        Resource::Scalar mem;
        cpus.set_value(-1);
        mem.set_value(-1);
        cpus = resources.getScalar("cpus", cpus);
        mem = resources.getScalar("mem", mem);

        state::Task* task =
          new state::Task(t->task_id().value(), t->name(),
                          TaskState_descriptor()->FindValueByNumber(t->state())->name(),
                          cpus.value(), mem.value());

        framework->tasks.push_back(task);
      }
    }
  }

  return state;
}


void Slave::operator () ()
{
  LOG(INFO) << "Slave started at " << self();
  LOG(INFO) << "Slave resources: " << resources;

  // Get our hostname
  char buf[256];
  gethostname(buf, sizeof(buf));
  hostent* he = gethostbyname2(buf, AF_INET);
  string hostname = he->h_name;

  // Check and see if we have a different public DNS name. Normally
  // this is our hostname, but on EC2 we look for the MESOS_PUBLIC_DNS
  // environment variable. This allows the master to display our
  // public name in its web UI.
  string public_hostname = hostname;
  if (getenv("MESOS_PUBLIC_DNS") != NULL) {
    public_hostname = getenv("MESOS_PUBLIC_DNS");
  }

  // Initialize slave info.
  slave.set_hostname(hostname);
  slave.set_public_hostname(public_hostname);
  slave.mutable_resources()->MergeFrom(resources);

  // Initialize isolation module.
  isolationModule->initialize(this);

  while (true) {
    serve(1);
    if (name() == process::TERMINATE) {
      LOG(INFO) << "Asked to shut down by " << from();
      foreachpaircopy (_, Framework* framework, frameworks) {
        killFramework(framework);
      }
      return;
    }
  }
}


void Slave::initialize()
{
  install(NEW_MASTER_DETECTED, &Slave::newMasterDetected,
          &NewMasterDetectedMessage::pid);

  install(NO_MASTER_DETECTED, &Slave::noMasterDetected);

  install(M2S_REGISTER_REPLY, &Slave::registerReply,
          &SlaveRegisteredMessage::slave_id);

  install(M2S_REREGISTER_REPLY, &Slave::reregisterReply,
          &SlaveRegisteredMessage::slave_id);

  install(M2S_RUN_TASK, &Slave::runTask,
          &RunTaskMessage::framework,
          &RunTaskMessage::framework_id,
          &RunTaskMessage::pid,
          &RunTaskMessage::task);

  install(M2S_KILL_TASK, &Slave::killTask,
          &KillTaskMessage::framework_id,
          &KillTaskMessage::task_id);

  install(M2S_KILL_FRAMEWORK, &Slave::killFramework,
          &KillFrameworkMessage::framework_id);

  install(M2S_FRAMEWORK_MESSAGE, &Slave::schedulerMessage,
          &FrameworkMessageMessage::slave_id,
          &FrameworkMessageMessage::framework_id,
          &FrameworkMessageMessage::executor_id,
          &FrameworkMessageMessage::data);

  install(M2S_UPDATE_FRAMEWORK, &Slave::updateFramework,
          &UpdateFrameworkMessage::framework_id,
          &UpdateFrameworkMessage::pid);

  install(M2S_STATUS_UPDATE_ACK, &Slave::statusUpdateAck,
          &StatusUpdateAckMessage::framework_id,
          &StatusUpdateAckMessage::slave_id,
          &StatusUpdateAckMessage::task_id);

  install(E2S_REGISTER_EXECUTOR, &Slave::registerExecutor,
          &RegisterExecutorMessage::framework_id,
          &RegisterExecutorMessage::executor_id);

  install(E2S_STATUS_UPDATE, &Slave::statusUpdate,
          &StatusUpdateMessage::framework_id,
          &StatusUpdateMessage::status);

  install(E2S_FRAMEWORK_MESSAGE, &Slave::executorMessage,
          &FrameworkMessageMessage::slave_id,
          &FrameworkMessageMessage::framework_id,
          &FrameworkMessageMessage::executor_id,
          &FrameworkMessageMessage::data);

  install(PING, &Slave::ping);

  install(process::TIMEOUT, &Slave::timeout);

  install(process::EXITED, &Slave::exited);
}


void Slave::newMasterDetected(const string& pid)
{
  LOG(INFO) << "New master at " << pid;

  master = pid;
  link(master);

  if (slaveId == "") {
    // Slave started before master.
    MSG<S2M_REGISTER_SLAVE> out;
    out.mutable_slave()->MergeFrom(slave);
    send(master, out);
  } else {
    // Re-registering, so send tasks running.
    MSG<S2M_REREGISTER_SLAVE> out;
    out.mutable_slave_id()->MergeFrom(slaveId);
    out.mutable_slave()->MergeFrom(slave);

    foreachpair (_, Framework* framework, frameworks) {
      foreachpair (_, Executor* executor, framework->executors) {
        foreachpair (_, Task* task, executor->tasks) {
          out.add_tasks()->MergeFrom(*task);
        }
      }
    }

    send(master, out);
  }
}


void Slave::noMasterDetected()
{
  LOG(INFO) << "Lost master(s) ... waiting";
}


void Slave::registerReply(const SlaveID& slaveId)
{
  LOG(INFO) << "Registered with master; given slave ID " << slaveId;
  this->slaveId = slaveId;
}


void Slave::reregisterReply(const SlaveID& slaveId)
{
  LOG(INFO) << "Re-registered with master";

  if (!(this->slaveId == slaveId)) {
    LOG(FATAL) << "Slave re-registered but got wrong ID";
  }
}


void Slave::runTask(const FrameworkInfo& frameworkInfo,
                    const FrameworkID& frameworkId,
                    const string& pid,
                    const TaskDescription& task)
{
  LOG(INFO) << "Got assigned task " << task.task_id()
            << " for framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    framework = new Framework(frameworkId, frameworkInfo, pid);
    frameworks[frameworkId] = framework;
  }

  // Either send the task to an executor or start a new executor
  // and queue the task until the executor has started.
  Executor* executor = task.has_executor()
    ? framework->getExecutor(task.executor().executor_id())
    : framework->getExecutor(framework->info.executor().executor_id());
        
  if (executor != NULL) {
    if (!executor->pid) {
      // Queue task until the executor starts up.
      executor->queuedTasks.push_back(task);
    } else {
      // Add the task to the executor.
      executor->addTask(task);

      MSG<S2E_RUN_TASK> out;
      out.mutable_framework()->MergeFrom(framework->info);
      out.mutable_framework_id()->MergeFrom(framework->frameworkId);
      out.set_pid(framework->pid);
      out.mutable_task()->MergeFrom(task);
      send(executor->pid, out);
      isolationModule->resourcesChanged(framework, executor);
    }
  } else {
    // Launch an executor for this task.
    if (task.has_executor()) {
      executor = framework->createExecutor(task.executor());
    } else {
      executor = framework->createExecutor(framework->info.executor());
    }

    // Queue task until the executor starts up.
    executor->queuedTasks.push_back(task);

    // Tell the isolation module to launch the executor.
    isolationModule->launchExecutor(framework, executor);
  }
}


void Slave::killTask(const FrameworkID& frameworkId,
                     const TaskID& taskId)
{
  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    // Tell the executor to kill the task if it is up and
    // running, otherwise, consider the task lost.
    Executor* executor = framework->getExecutor(taskId);
    if (executor == NULL || !executor->pid) {
      // Update the resources locally, if an executor comes up
      // after this then it just won't receive this task.
      executor->removeTask(taskId);
      isolationModule->resourcesChanged(framework, executor);

      MSG<S2M_STATUS_UPDATE> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      TaskStatus *status = out.mutable_status();
      status->mutable_task_id()->MergeFrom(taskId);
      status->mutable_slave_id()->MergeFrom(slaveId);
      status->set_state(TASK_LOST);
      send(master, out);

      double deadline = elapsed() + STATUS_UPDATE_RETRY_TIMEOUT;
      framework->statuses[deadline][status->task_id()] = *status;
    } else {
      // Otherwise, send a message to the executor and wait for
      // it to send us a status update.
      MSG<S2E_KILL_TASK> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_task_id()->MergeFrom(taskId);
      send(executor->pid, out);
    }
  } else {
    LOG(WARNING) << "Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no such framework is running";

    MSG<S2M_STATUS_UPDATE> out;
    out.mutable_framework_id()->MergeFrom(frameworkId);
    TaskStatus *status = out.mutable_status();
    status->mutable_task_id()->MergeFrom(taskId);
    status->mutable_slave_id()->MergeFrom(slaveId);
    status->set_state(TASK_LOST);
    send(master, out);

    double deadline = elapsed() + STATUS_UPDATE_RETRY_TIMEOUT;
    framework->statuses[deadline][status->task_id()] = *status;
  }
}


void Slave::killFramework(const FrameworkID& frameworkId)
{
  LOG(INFO) << "Asked to kill framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    killFramework(framework);
  }
}


void Slave::schedulerMessage(const SlaveID& slaveId,
			     const FrameworkID& frameworkId,
			     const ExecutorID& executorId,
                             const string& data)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Executor* executor = framework->getExecutor(executorId);
    if (executor == NULL) {
      LOG(WARNING) << "Dropping message for executor '"
                   << executorId << "' of framework " << frameworkId
                   << " because executor does not exist";
    } else if (!executor->pid) {
      // TODO(*): If executor is not started, queue framework message?
      // (It's probably okay to just drop it since frameworks can have
      // the executor send a message to the master to say when it's ready.)
      LOG(WARNING) << "Dropping message for executor '"
                   << executorId << "' of framework " << frameworkId
                   << " because executor is not running";
    } else {
      MSG<S2E_FRAMEWORK_MESSAGE> out;
      out.mutable_slave_id()->MergeFrom(slaveId);
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_executor_id()->MergeFrom(executorId);
      out.set_data(data);
      send(executor->pid, out);
    }
  } else {
    LOG(WARNING) << "Dropping message for framework "<< frameworkId
                 << " because it does not exist";
  }
}


void Slave::updateFramework(const FrameworkID& frameworkId,
                            const string& pid)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Updating framework " << frameworkId
              << " pid to " <<pid;
    framework->pid = pid;
  }
}


void Slave::statusUpdateAck(const FrameworkID& frameworkId,
                            const SlaveID& slaveId,
                            const TaskID& taskId)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    foreachpair (double deadline, _, framework->statuses) {
      if (framework->statuses[deadline].count(taskId) > 0) {
        LOG(INFO) << "Got acknowledgement of status update"
                  << " for task " << taskId
                  << " of framework " << framework->frameworkId;
        framework->statuses[deadline].erase(taskId);
        break;
      }
    }
  }
}


void Slave::registerExecutor(const FrameworkID& frameworkId,
                             const ExecutorID& executorId)
{
  LOG(INFO) << "Got registration for executor '" << executorId
            << "' of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Executor* executor = framework->getExecutor(executorId);

    // Check the status of the executor.
    if (executor == NULL) {
      LOG(WARNING) << "Not expecting executor '" << executorId
                   << "' of framework " << frameworkId;
      send(from(), S2E_KILL_EXECUTOR);
    } else if (executor->pid != UPID()) {
      LOG(WARNING) << "Not good, executor '" << executorId
                   << "' of framework " << frameworkId
                   << " is already running";
      send(from(), S2E_KILL_EXECUTOR);
    } else {
      // Save the pid for the executor.
      executor->pid = from();

      // Now that the executor is up, set its resource limits.
      isolationModule->resourcesChanged(framework, executor);

      // Tell executor it's registered and give it any queued tasks.
      MSG<S2E_REGISTER_REPLY> out;
      ExecutorArgs* args = out.mutable_args();
      args->mutable_framework_id()->MergeFrom(framework->frameworkId);
      args->mutable_executor_id()->MergeFrom(executor->info.executor_id());
      args->mutable_slave_id()->MergeFrom(slaveId);
      args->set_hostname(slave.hostname());
      args->set_data(framework->info.executor().data());
      send(executor->pid, out);
      sendQueuedTasks(framework, executor);
    }
  } else {
    // Framework is gone; tell the executor to exit.
    LOG(WARNING) << "Framework " << frameworkId
                 << " does not exist (it may have been killed),"
                 << " telling executor to exit";

    // TODO(benh): Don't we also want to tell the isolation
    // module to shut this guy down!
    send(from(), S2E_KILL_EXECUTOR);
  }
}


void Slave::statusUpdate(const FrameworkID& frameworkId,
                         const TaskStatus& status)
{
  LOG(INFO) << "Status update: task " << status.task_id()
            << " of framework " << frameworkId
            << " is now in state "
            << TaskState_descriptor()->FindValueByNumber(status.state())->name();

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Executor* executor = framework->getExecutor(status.task_id());
    if (executor != NULL) {
      if (status.state() == TASK_FINISHED ||
          status.state() == TASK_FAILED ||
          status.state() == TASK_KILLED ||
          status.state() == TASK_LOST) {
        executor->removeTask(status.task_id());
        isolationModule->resourcesChanged(framework, executor);
      }

      // Send message and record the status for possible resending.
      MSG<S2M_STATUS_UPDATE> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_status()->MergeFrom(status);
      send(master, out);

      double deadline = elapsed() + STATUS_UPDATE_RETRY_TIMEOUT;
      framework->statuses[deadline][status.task_id()] = status;
    } else {
      LOG(WARNING) << "Status update error: couldn't lookup "
                   << "executor for framework " << frameworkId;
    }
  } else {
    LOG(WARNING) << "Status update error: couldn't lookup "
                 << "framework " << frameworkId;
  }
}


void Slave::executorMessage(const SlaveID& slaveId,
			    const FrameworkID& frameworkId,
			    const ExecutorID& executorId,
                            const string& data)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Sending message for framework " << frameworkId
              << " to " << framework->pid;

    // TODO(benh): This is weird, sending an M2F message.
    MSG<M2F_FRAMEWORK_MESSAGE> out;
    out.mutable_slave_id()->MergeFrom(slaveId);
    out.mutable_framework_id()->MergeFrom(frameworkId);
    out.mutable_executor_id()->MergeFrom(executorId);
    out.set_data(data);
    send(framework->pid, out);
  }
}


void Slave::ping()
{
  send(from(), PONG);
}


void Slave::timeout()
{
  // Check and see if we should re-send any status updates.
  foreachpair (_, Framework* framework, frameworks) {
    foreachpair (double deadline, _, framework->statuses) {
      if (deadline <= elapsed()) {
        foreachpair (_, const TaskStatus& status, framework->statuses[deadline]) {
          LOG(WARNING) << "Resending status update"
                       << " for task " << status.task_id()
                       << " of framework " << framework->frameworkId;
          MSG<S2M_STATUS_UPDATE> out;
          out.mutable_framework_id()->MergeFrom(framework->frameworkId);
          out.mutable_status()->MergeFrom(status);
          send(master, out);
        }
      }
    }
  }
}

void Slave::exited()
{
  LOG(INFO) << "Process exited: " << from();

  if (from() == master) {
    LOG(WARNING) << "Master disconnected! "
                 << "Waiting for a new master to be elected.";
    // TODO(benh): After so long waiting for a master, commit suicide.
  }
}




Framework* Slave::getFramework(const FrameworkID& frameworkId)
{
  if (frameworks.count(frameworkId) > 0) {
    return frameworks[frameworkId];
  }

  return NULL;
}


// Send any tasks queued up for the given framework to its executor
// (needed if we received tasks while the executor was starting up)
void Slave::sendQueuedTasks(Framework* framework, Executor* executor)
{
  LOG(INFO) << "Flushing queued tasks for framework "
            << framework->frameworkId;

  CHECK(executor->pid != UPID());

  foreach (const TaskDescription& task, executor->queuedTasks) {
    // Add the task to the executor.
    executor->addTask(task);

    MSG<S2E_RUN_TASK> out;
    out.mutable_framework()->MergeFrom(framework->info);
    out.mutable_framework_id()->MergeFrom(framework->frameworkId);
    out.set_pid(framework->pid);
    out.mutable_task()->MergeFrom(task);
    send(executor->pid, out);
  }

  executor->queuedTasks.clear();
}


// Kill a framework (including its executor if killExecutor is true).
void Slave::killFramework(Framework *framework, bool killExecutors)
{
  LOG(INFO) << "Cleaning up framework " << framework->frameworkId;

  // Shutdown all executors of this framework.
  foreachpaircopy (const ExecutorID& executorId, Executor* executor, framework->executors) {
    if (killExecutors) {
      LOG(INFO) << "Killing executor '" << executorId
                << "' of framework " << framework->frameworkId;

      send(executor->pid, S2E_KILL_EXECUTOR);

      // TODO(benh): There really isn't ANY time between when an
      // executor gets a S2E_KILL_EXECUTOR message and the isolation
      // module goes and kills it. We should really think about making
      // the semantics of this better.

      isolationModule->killExecutor(framework, executor);
    }

    framework->destroyExecutor(executorId);
  }

  frameworks.erase(framework->frameworkId);
  delete framework;
}


// Called by isolation module when an executor process exits
// TODO(benh): Make this callback be a message so that we can avoid
// race conditions.
void Slave::executorExited(const FrameworkID& frameworkId, const ExecutorID& executorId, int result)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Executor* executor = framework->getExecutor(executorId);
    if (executor != NULL) {
      LOG(INFO) << "Exited executor '" << executorId
                << "' of framework " << frameworkId
                << " with result " << result;

      MSG<S2M_EXITED_EXECUTOR> out;
      out.mutable_slave_id()->MergeFrom(slaveId);
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_executor_id()->MergeFrom(executorId);
      out.set_result(result);
      send(master, out);

      framework->destroyExecutor(executorId);

      // TODO(benh): When should we kill the presence of an entire
      // framework on a slave?
      if (framework->executors.size() == 0) {
        killFramework(framework);
      }
    } else {
      LOG(WARNING) << "UNKNOWN executor '" << executorId
                   << "' of framework " << frameworkId
                   << " has exited with result " << result;
    }
  } else {
    LOG(WARNING) << "UNKNOWN executor '" << executorId
                 << "' of UNKNOWN framework " << frameworkId
                 << " has exited with result " << result;
  }
};


string Slave::getUniqueWorkDirectory(const FrameworkID& frameworkId,
                                     const ExecutorID& executorId)
{
  string workDir;
  if (conf.contains("work_dir")) {
    workDir = conf["work_dir"];
  } else if (conf.contains("home")) {
    workDir = conf["home"] + "/work";
  } else {
    workDir = "work";
  }

  ostringstream os(std::ios_base::app | std::ios_base::out);
  os << workDir << "/slave-" << slaveId
     << "/fw-" << frameworkId << "-" << executorId;

  // Find a unique directory based on the path given by the slave
  // (this is because we might launch multiple executors from the same
  // framework on this slave).
  os << "/";

  string dir;
  dir = os.str();

  for (int i = 0; i < INT_MAX; i++) {
    os << i;
    if (opendir(os.str().c_str()) == NULL && errno == ENOENT)
      break;
    os.str(dir);
  }

  return os.str();
}


const Configuration& Slave::getConfiguration()
{
  return conf;
}
