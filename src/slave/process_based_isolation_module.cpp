#include <signal.h>

#include <map>

#include <process/dispatch.hpp>

#include "process_based_isolation_module.hpp"

#include "common/foreach.hpp"
#include "common/type_utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using namespace process;

using launcher::ExecutorLauncher;

using std::map;
using std::string;

using process::wait; // Necessary on some OS's to disambiguate.


ProcessBasedIsolationModule::ProcessBasedIsolationModule()
  : initialized(false)
{
  // Spawn the reaper, note that it might send us a message before we
  // actually get spawned ourselves, but that's okay, the message will
  // just get dropped.
  reaper = new Reaper();
  spawn(reaper);
  dispatch(reaper, &Reaper::addProcessExitedListener, this);
}


ProcessBasedIsolationModule::~ProcessBasedIsolationModule()
{
  CHECK(reaper != NULL);
  terminate(reaper);
  wait(reaper);
  delete reaper;
}


void ProcessBasedIsolationModule::initialize(
    const Configuration& _conf,
    bool _local,
    const PID<Slave>& _slave)
{
  conf = _conf;
  local = _local;
  slave = _slave;

  initialized = true;
}


void ProcessBasedIsolationModule::launchExecutor(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const string& directory)
{
  if (!initialized) {
    LOG(FATAL) << "Cannot launch executors before initialization!";
  }

  LOG(INFO) << "Launching '" << executorInfo.uri()
            << "' for executor '" << executorInfo.executor_id()
            << "' of framework " << frameworkId;

  pid_t pid;
  if ((pid = fork()) == -1) {
    PLOG(FATAL) << "Failed to fork to launch new executor";
  }

  if (pid) {
    // In parent process, record the pgid for killpg later.
    LOG(INFO) << "Started executor, OS pid = " << pid;
    pgids[frameworkId][executorInfo.executor_id()] = pid;

    // Tell the slave this executor has started.
    dispatch(slave, &Slave::executorStarted,
             frameworkId, executorInfo.executor_id(), pid);
  } else {
    // In child process, make cleanup easier.
    if ((pid = setsid()) == -1) {
      PLOG(FATAL) << "Failed to put executor in own session";
    }

    ExecutorLauncher* launcher = 
      createExecutorLauncher(frameworkId, frameworkInfo,
                             executorInfo, directory);

    launcher->run();
  }
}


void ProcessBasedIsolationModule::killExecutor(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  if (!pgids.contains(frameworkId) ||
      !pgids[frameworkId].contains(executorId)) {
    LOG(ERROR) << "ERROR! Asked to kill an unknown executor!";
    return;
  }

  if (pgids[frameworkId][executorId] != -1) {
    // TODO(benh): Consider sending a SIGTERM, then after so much time
    // if it still hasn't exited do a SIGKILL (can use a libprocess
    // process for this). This might not be necessary because we have
    // higher-level semantics via the first shut down phase that gets
    // initiated by the slave.
    LOG(INFO) << "Sending SIGKILL to process group "
              << pgids[frameworkId][executorId];

    killpg(pgids[frameworkId][executorId], SIGKILL);

    if (pgids[frameworkId].size() == 1) {
      pgids.erase(frameworkId);
    } else {
      pgids[frameworkId].erase(executorId);
    }

    // NOTE: Both frameworkId and executorId are no longer valid
    // because they have just been deleted above!

    // TODO(benh): Kill all of the process's descendants? Perhaps
    // create a new libprocess process that continually tries to kill
    // all the processes that are a descendant of the executor, trying
    // to kill the executor last ... maybe this is just too much of a
    // burden?
  }
}


void ProcessBasedIsolationModule::resourcesChanged(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Resources& resources)
{
  // Do nothing; subclasses may override this.
}


ExecutorLauncher* ProcessBasedIsolationModule::createExecutorLauncher(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const string& directory)
{
  // Create a map of parameters for the executor launcher.
  map<string, string> params;

  for (int i = 0; i < executorInfo.params().param_size(); i++) {
    params[executorInfo.params().param(i).key()] = 
      executorInfo.params().param(i).value();
  }

  return new ExecutorLauncher(frameworkId,
                              executorInfo.executor_id(),
                              executorInfo.uri(),
                              frameworkInfo.user(),
                              directory,
                              slave,
                              conf.get("frameworks_home", ""),
                              conf.get("home", ""),
                              conf.get("hadoop_home", ""),
                              !local,
                              conf.get("switch_user", true),
			      "",
                              params);
}


void ProcessBasedIsolationModule::processExited(pid_t pid, int status)
{
  foreachkey (const FrameworkID& frameworkId, pgids) {
    foreachpair (const ExecutorID& executorId, pid_t pgid, pgids[frameworkId]) {
      if (pgid == pid) {
        LOG(INFO) << "Telling slave of lost executor " << executorId
                  << " of framework " << frameworkId;

        dispatch(slave, &Slave::executorExited,
                 frameworkId, executorId, status);

        // Try and cleanup after the executor.
        killExecutor(frameworkId, executorId);
	return;
      }
    }
  }
}
