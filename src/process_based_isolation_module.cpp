#include "process_based_isolation_module.hpp"

#include "foreach.hpp"

using std::cerr;
using std::cout;
using std::endl;
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

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;


ProcessBasedIsolationModule::ProcessBasedIsolationModule()
  : initialized(false) {}


ProcessBasedIsolationModule::~ProcessBasedIsolationModule()
{
  // We need to wait until the reaper has completed because it
  // accesses 'this' in order to make callbacks ... deleting 'this'
  // could thus lead to a seg fault!
  if (initialized) {
    CHECK(reaper != NULL);
    Process::post(reaper->self(), SHUTDOWN_REAPER);
    Process::wait(reaper->self());
    delete reaper;
  }
}


void ProcessBasedIsolationModule::initialize(Slave *slave)
{
  this->slave = slave;
  reaper = new Reaper(this);
  Process::spawn(reaper);
  initialized = true;
}


void ProcessBasedIsolationModule::startExecutor(Framework* fw)
{
  if (!initialized)
    LOG(FATAL) << "Cannot launch executors before initialization!";

  LOG(INFO) << "Starting executor for framework " << fw->id << ": "
            << fw->executorInfo.uri;

  pid_t pid;
  if ((pid = fork()) == -1)
    PLOG(FATAL) << "Failed to fork to launch new executor";

  if (pid) {
    // In parent process, record the pgid for killpg later.
    LOG(INFO) << "Started executor, OS pid = " << pid;
    pgids[fw->id] = pid;
    fw->executorStatus = "PID: " + lexical_cast<string>(pid);
  } else {
    // In child process, make cleanup easier.
//     if (setpgid(0, 0) < 0)
//       PLOG(FATAL) << "Failed to put executor in own process group";
    if ((pid = setsid()) == -1)
      PLOG(FATAL) << "Failed to put executor in own session";

    createExecutorLauncher(fw)->run();
  }
}


void ProcessBasedIsolationModule::killExecutor(Framework* fw)
{
  if (pgids[fw->id] != -1) {
    // TODO(benh): Consider sending a SIGTERM, then after so much time
    // if it still hasn't exited do a SIGKILL (can use a libprocess
    // process for this).
    LOG(INFO) << "Sending SIGKILL to gpid " << pgids[fw->id];
    killpg(pgids[fw->id], SIGKILL);
    pgids[fw->id] = -1;
    fw->executorStatus = "No executor running";

    // TODO(benh): Kill all of the process's descendants? Perhaps
    // create a new libprocess process that continually tries to kill
    // all the processes that are a descendant of the executor, trying
    // to kill the executor last ... maybe this is just too much of a
    // burden?

    pgids.erase(fw->id);
  }
}


void ProcessBasedIsolationModule::resourcesChanged(Framework* fw)
{
  // Do nothing; subclasses may override this.
}


ExecutorLauncher* ProcessBasedIsolationModule::createExecutorLauncher(
    Framework* fw)
{
  return new ExecutorLauncher(fw->id,
                              fw->executorInfo.uri,
                              fw->user,
                              slave->getWorkDirectory(fw->id),
                              slave->self(),
                              slave->getConf().get("hadoop_home", ""),
                              !slave->local,
                              fw->executorInfo.params);
}


ProcessBasedIsolationModule::Reaper::Reaper(ProcessBasedIsolationModule* m)
  : module(m)
{}


void ProcessBasedIsolationModule::Reaper::operator () ()
{
  link(module->slave->self());
  while (true) {
    switch (receive(1)) {
    case PROCESS_TIMEOUT: {
      // Check whether any child process has exited
      pid_t pid;
      int status;
      if ((pid = waitpid((pid_t) -1, &status, WNOHANG)) > 0) {
        foreachpair (FrameworkID fid, pid_t pgid, module->pgids) {
          if (pgid == pid) {
            // Kill the process group to clean up the tasks.
            LOG(INFO) << "Sending SIGKILL to gpid " << pgid;
            killpg(pgid, SIGKILL);
            module->pgids[fid] = -1;
            LOG(INFO) << "Telling slave of lost framework " << fid;
            // TODO(benh): This is broken if/when libprocess is parallel!
            module->slave->executorExited(fid, status);
            module->pgids.erase(fid);
            break;
          }
        }
      }
      break;
    }
    case SHUTDOWN_REAPER:
    case PROCESS_EXIT:
      return;
    }
  }
}

