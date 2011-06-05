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
    isolationModule(_isolationModule), heart(NULL) {}


Slave::Slave(const Configuration& _conf, bool _local,
             IsolationModule* _isolationModule)
  : conf(_conf), local(_local),
    isolationModule(_isolationModule), heart(NULL)
{
  resources =
    Resources::parse(conf.get<string>("resources", "cpus:1;mem:1024"));
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


state::SlaveState *Slave::getState()
{
  Resources resources(resources);
  Resource::Scalar cpus;
  Resource::Scalar mem;
  cpus.set_value(-1);
  mem.set_value(-1);
  cpus = resources.getScalar("cpus", cpus);
  mem = resources.getScalar("mem", mem);

  state::SlaveState *state =
    new state::SlaveState(BUILD_DATE, BUILD_USER, slaveId.value(),
                          cpus.value(), mem.value(), self(), master);

//   foreachpair (_, Framework *f, frameworks) {

//     foreachpair (_, Executor* e, f->executors) {

//       Resources resources(e->resources);
//       Resource::Scalar cpus;
//       Resource::Scalar mem;
//       cpus.set_value(-1);
//       mem.set_value(-1);
//       cpus = resources.getScalar("cpus", cpus);
//       mem = resources.getScalar("mem", mem);

//     state::Framework *framework =
//       new state::Framework(f->frameworkId.value(), f->info.name(),
//                            f->info.executor().uri(), f->executorStatus,
//                            cpus.value(), mem.value());

//     state->frameworks.push_back(framework);

//     foreachpair(_, Task *t, f->tasks) {
//       Resources resources(t->resources());
//       Resource::Scalar cpus;
//       Resource::Scalar mem;
//       cpus.set_value(-1);
//       mem.set_value(-1);
//       cpus = resources.getScalar("cpus", cpus);
//       mem = resources.getScalar("mem", mem);

//       state::Task *task =
//         new state::Task(t->task_id().value(), t->name(),
//                         TaskState_descriptor()->FindValueByNumber(t->state())->name(),
//                         cpus.value(), mem.value());

//       framework->tasks.push_back(task);
//     }
//   }

  return state;
}


void Slave::operator () ()
{
  LOG(INFO) << "Slave started at " << self();
  LOG(INFO) << "Slave resources: " << resources;

  // Get our hostname
  char buf[256];
  gethostname(buf, sizeof(buf));
  hostent *he = gethostbyname2(buf, AF_INET);
  string hostname = he->h_name;

  // Get our public DNS name. Normally this is our hostname, but on EC2
  // we look for the MESOS_PUBLIC_DNS environment variable. This allows
  // the master to display our public name in its web UI.
  string public_hostname = hostname;
  if (getenv("MESOS_PUBLIC_DNS") != NULL) {
    public_hostname = getenv("MESOS_PUBLIC_DNS");
  }

  SlaveInfo slave;
  slave.set_hostname(hostname);
  slave.set_public_hostname(public_hostname);
  slave.mutable_resources()->MergeFrom(resources);

  // Initialize isolation module.
  isolationModule->initialize(this);

  while (true) {
    switch (receive(1)) {
      case NEW_MASTER_DETECTED: {
        const MSG<NEW_MASTER_DETECTED>& msg = message();

	LOG(INFO) << "New master at " << msg.pid();

	master = msg.pid();
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
                out.add_task()->MergeFrom(*task);
              }
            }
          }

	  send(master, out);
	}
	break;
      }
	
      case NO_MASTER_DETECTED: {
	LOG(INFO) << "Lost master(s) ... waiting";
	break;
      }

      case MASTER_DETECTION_FAILURE: {
	LOG(FATAL) << "Cannot reliably detect master ... committing suicide!";
	break;
      }

      case M2S_REGISTER_REPLY: {
        const MSG<M2S_REGISTER_REPLY>& msg = message();
        slaveId = msg.slave_id();

        LOG(INFO) << "Registered with master; given slave ID " << slaveId;

        heart = new Heart(master, self(), slaveId, msg.heartbeat_interval());
        link(spawn(heart));
        break;
      }
      
      case M2S_REREGISTER_REPLY: {
        const MSG<M2S_REREGISTER_REPLY>& msg = message();

        LOG(INFO) << "Re-registered with master";

        if (!(slaveId == msg.slave_id())) {
          LOG(FATAL) << "Slave re-registered but got wrong ID";
        }

        if (heart != NULL) {
          send(heart->self(), MESOS_MSGID);
          wait(heart->self());
          delete heart;
        }

        heart = new Heart(master, self(), slaveId, msg.heartbeat_interval());
        link(spawn(heart));
        break;
      }
      
      case M2S_RUN_TASK: {
        const MSG<M2S_RUN_TASK>& msg = message();

        const TaskDescription& task = msg.task();

        LOG(INFO) << "Got assigned task " << task.task_id()
                  << " for framework " << msg.framework_id();

        Framework *framework = getFramework(msg.framework_id());
        if (framework == NULL) {
          framework =
            new Framework(msg.framework_id(), msg.framework(), msg.pid());
          frameworks[msg.framework_id()] = framework;
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
        break;
      }

      case M2S_KILL_TASK: {
        const MSG<M2S_KILL_TASK>& msg = message();

        LOG(INFO) << "Asked to kill task " << msg.task_id()
                  << " of framework " << msg.framework_id();

        Framework* framework = getFramework(msg.framework_id());
        if (framework != NULL) {
          // Tell the executor to kill the task if it is up and
          // running, otherwise, consider the task lost.
          Executor* executor = framework->getExecutor(msg.task_id());
          if (executor == NULL || !executor->pid) {
            // Update the resources locally, if an executor comes up
            // after this then it just won't receive this task.
            executor->removeTask(msg.task_id());
            isolationModule->resourcesChanged(framework, executor);

            MSG<S2M_STATUS_UPDATE> out;
            out.mutable_framework_id()->MergeFrom(msg.framework_id());
            TaskStatus *status = out.mutable_status();
            status->mutable_task_id()->MergeFrom(msg.task_id());
            status->mutable_slave_id()->MergeFrom(slaveId);
            status->set_state(TASK_LOST);
            send(master, out);

            double deadline = elapsed() + STATUS_UPDATE_RETRY_TIMEOUT;
            framework->statuses[deadline][status->task_id()] = *status;
          } else {
            // Otherwise, send a message to the executor and wait for
            // it to send us a status update.
            MSG<S2E_KILL_TASK> out;
            out.mutable_framework_id()->MergeFrom(msg.framework_id());
            out.mutable_task_id()->MergeFrom(msg.task_id());
            send(executor->pid, out);
          }
        } else {
          LOG(WARNING) << "Cannot kill task " << msg.task_id()
                       << " of framework " << msg.framework_id()
                       << " because no such framework is running";

          MSG<S2M_STATUS_UPDATE> out;
          out.mutable_framework_id()->MergeFrom(msg.framework_id());
          TaskStatus *status = out.mutable_status();
          status->mutable_task_id()->MergeFrom(msg.task_id());
          status->mutable_slave_id()->MergeFrom(slaveId);
          status->set_state(TASK_LOST);
          send(master, out);

          double deadline = elapsed() + STATUS_UPDATE_RETRY_TIMEOUT;
          framework->statuses[deadline][status->task_id()] = *status;
        }
        break;
      }

      case M2S_KILL_FRAMEWORK: {
        const MSG<M2S_KILL_FRAMEWORK>&msg = message();

        LOG(INFO) << "Asked to kill framework " << msg.framework_id();

        Framework *framework = getFramework(msg.framework_id());
        if (framework != NULL)
          killFramework(framework);
        break;
      }

      case M2S_FRAMEWORK_MESSAGE: {
        const MSG<M2S_FRAMEWORK_MESSAGE>&msg = message();

        Framework* framework = getFramework(msg.framework_id());
        if (framework != NULL) {
          const FrameworkMessage& message = msg.message();

          Executor* executor = framework->getExecutor(message.executor_id());
          if (executor == NULL) {
            LOG(WARNING) << "Dropping message for executor '"
                         << message.executor_id() << "' of framework "
                         << msg.framework_id()
                         << " because executor does not exist";
          } else if (!executor->pid) {
            // TODO(*): If executor is not started, queue framework message?
            // (It's probably okay to just drop it since frameworks can have
            // the executor send a message to the master to say when it's ready.)
            LOG(WARNING) << "Dropping message for executor '"
                         << message.executor_id() << "' of framework "
                         << msg.framework_id()
                         << " because executor is not running";
          } else {
            MSG<S2E_FRAMEWORK_MESSAGE> out;
            out.mutable_framework_id()->MergeFrom(msg.framework_id());
            out.mutable_message()->MergeFrom(message);
            send(executor->pid, out);
          }
        } else {
          LOG(WARNING) << "Dropping message for framework "
                       << msg.framework_id()
                       << " because it does not exist";
        }
        break;
      }

      case M2S_UPDATE_FRAMEWORK: {
        const MSG<M2S_UPDATE_FRAMEWORK>&msg = message();

        Framework *framework = getFramework(msg.framework_id());
        if (framework != NULL) {
          LOG(INFO) << "Updating framework " << msg.framework_id()
                    << " pid to " << msg.pid();
          framework->pid = msg.pid();
        }
        break;
      }

      case M2S_STATUS_UPDATE_ACK: {
        const MSG<M2S_STATUS_UPDATE_ACK>& msg = message();

        Framework* framework = getFramework(msg.framework_id());
        if (framework != NULL) {
          foreachpair (double deadline, _, framework->statuses) {
            if (framework->statuses[deadline].count(msg.task_id()) > 0) {
              LOG(INFO) << "Got acknowledgement of status update"
                        << " for task " << msg.task_id()
                        << " of framework " << framework->frameworkId;
              framework->statuses[deadline].erase(msg.task_id());
              break;
            }
          }
        }
        break;
      }

      case E2S_REGISTER_EXECUTOR: {
        const MSG<E2S_REGISTER_EXECUTOR>& msg = message();

        LOG(INFO) << "Got registration for executor '"
                  << msg.executor_id() << "' of framework "
                  << msg.framework_id();

        Framework* framework = getFramework(msg.framework_id());
        if (framework != NULL) {
          Executor* executor = framework->getExecutor(msg.executor_id());

          // Check the status of the executor.
          if (executor == NULL) {
            LOG(WARNING) << "Not expecting executor '" << msg.executor_id()
                         << "' of framework " << msg.framework_id();
            send(from(), S2E_KILL_EXECUTOR);
          } else if (executor->pid != PID()) {
            LOG(WARNING) << "Not good, executor '" << msg.executor_id()
                         << "' of framework " << msg.framework_id()
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
            args->set_name(framework->info.name());
            args->mutable_slave_id()->MergeFrom(slaveId);
            args->set_hostname(hostname);
            args->set_data(framework->info.executor().data());
            send(executor->pid, out);
            sendQueuedTasks(framework, executor);
          }
        } else {
          // Framework is gone; tell the executor to exit.
          LOG(WARNING) << "Framework " << msg.framework_id()
                       << " does not exist (it may have been killed),"
                       << " telling executor to exit";

          // TODO(benh): Don't we also want to tell the isolation
          // module to shut this guy down!
          send(from(), S2E_KILL_EXECUTOR);
        }
        break;
      }

      case E2S_STATUS_UPDATE: {
        const MSG<E2S_STATUS_UPDATE>& msg = message();

        const TaskStatus& status = msg.status();

	LOG(INFO) << "Status update: task " << status.task_id()
		  << " of framework " << msg.framework_id()
		  << " is now in state "
		  << TaskState_descriptor()->FindValueByNumber(status.state())->name();

        Framework *framework = getFramework(msg.framework_id());
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
            out.mutable_framework_id()->MergeFrom(msg.framework_id());
            out.mutable_status()->MergeFrom(status);
            send(master, out);

            double deadline = elapsed() + STATUS_UPDATE_RETRY_TIMEOUT;
            framework->statuses[deadline][status.task_id()] = status;
          } else {
            LOG(WARNING) << "Status update error: couldn't lookup "
                         << "executor for framework " << msg.framework_id();
          }
	} else {
          LOG(WARNING) << "Status update error: couldn't lookup "
                       << "framework " << msg.framework_id();
	}
        break;
      }

      case E2S_FRAMEWORK_MESSAGE: {
        const MSG<E2S_FRAMEWORK_MESSAGE>& msg = message();

        const FrameworkMessage& message = msg.message();

        Framework *framework = getFramework(msg.framework_id());
        if (framework != NULL) {
	  LOG(INFO) << "Sending message for framework "
                    << framework->frameworkId
		    << " to " << framework->pid;

          // TODO(benh): This is weird, sending an M2F message.
          MSG<M2F_FRAMEWORK_MESSAGE> out;
          out.mutable_framework_id()->MergeFrom(msg.framework_id());
          out.mutable_message()->MergeFrom(message);
          out.mutable_message()->mutable_slave_id()->MergeFrom(slaveId);
          send(framework->pid, out);
        }
        break;
      }

      case S2S_GET_STATE: {
        state::SlaveState *state = getState();
        MSG<S2S_GET_STATE_REPLY> out;
        out.set_pointer((char *) &state, sizeof(state));
        send(from(), out);
        break;
      }

      case M2S_SHUTDOWN: {
        LOG(INFO) << "Asked to shut down by master: " << from();
        foreachpaircopy (_, Framework *framework, frameworks) {
          killFramework(framework);
        }
        return;
      }

      case S2S_SHUTDOWN: {
        LOG(INFO) << "Asked to shut down by " << from();
        foreachpaircopy (_, Framework *framework, frameworks) {
          killFramework(framework);
        }
        return;
      }

      case PROCESS_EXIT: {
        LOG(INFO) << "Process exited: " << from();

        if (from() == master) {
	  LOG(WARNING) << "Master disconnected! "
		       << "Waiting for a new master to be elected.";
	  // TODO(benh): After so long waiting for a master, commit suicide.
	}
        break;
      }

      case PROCESS_TIMEOUT: {
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
        break;
      }

      default: {
        LOG(ERROR) << "Received unknown message (" << msgid()
                   << ") from " << from();
        break;
      }
    }
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

  CHECK(executor->pid != PID());

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
void Slave::executorExited(const FrameworkID& frameworkId, const ExecutorID& executorId, int status)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Executor* executor = framework->getExecutor(executorId);
    if (executor != NULL) {
      LOG(INFO) << "Exited executor '" << executorId
                << "' of framework " << frameworkId
                << " with status " << status;

      MSG<S2M_EXITED_EXECUTOR> out;
      out.mutable_slave_id()->MergeFrom(slaveId);
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_executor_id()->MergeFrom(executorId);
      out.set_status(status);
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
                   << " has exited with status " << status;
    }
  } else {
    LOG(WARNING) << "UNKNOWN executor '" << executorId
                 << "' of UNKNOWN framework " << frameworkId
                 << " has exited with status " << status;
  }
};


string Slave::getUniqueWorkDirectory(const FrameworkID& frameworkId)
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
  os << workDir << "/slave-" << slaveId << "/fw-" << frameworkId;

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
