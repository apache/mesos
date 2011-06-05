#include <map>
#include <vector>

#include "process_based_isolation_module.hpp"

#include "common/foreach.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using boost::lexical_cast;
using boost::unordered_map;
using boost::unordered_set;

using launcher::ExecutorLauncher;

using std::cerr;
using std::cout;
using std::endl;
using std::list;
using std::make_pair;
using std::map;
using std::ostringstream;
using std::pair;
using std::queue;
using std::string;
using std::vector;


ProcessBasedIsolationModule::ProcessBasedIsolationModule()
  : initialized(false) {}


ProcessBasedIsolationModule::~ProcessBasedIsolationModule()
{
  // We need to wait until the reaper has completed because it
  // accesses 'this' in order to make callbacks ... deleting 'this'
  // could thus lead to a seg fault!
  if (initialized) {
    CHECK(reaper != NULL);
    Process::post(reaper->self(), TERMINATE);
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


void ProcessBasedIsolationModule::launchExecutor(Framework* framework, Executor* executor)
{
  if (!initialized)
    LOG(FATAL) << "Cannot launch executors before initialization!";

  LOG(INFO) << "Starting executor for framework " << framework->frameworkId
            << ": " << executor->info.uri();

  pid_t pid;
  if ((pid = fork()) == -1)
    PLOG(FATAL) << "Failed to fork to launch new executor";

  if (pid) {
    // In parent process, record the pgid for killpg later.
    LOG(INFO) << "Started executor, OS pid = " << pid;
    pgids[framework->frameworkId][executor->info.executor_id()] = pid;
    executor->executorStatus = "PID: " + lexical_cast<string>(pid);
  } else {
    // In child process, make cleanup easier.
//     if (setpgid(0, 0) < 0)
//       PLOG(FATAL) << "Failed to put executor in own process group";
    if ((pid = setsid()) == -1)
      PLOG(FATAL) << "Failed to put executor in own session";
    
    createExecutorLauncher(framework, executor)->run();
  }
}


void ProcessBasedIsolationModule::killExecutor(Framework* framework, Executor* executor)
{
  if (pgids[framework->frameworkId][executor->info.executor_id()] != -1) {
    // TODO(benh): Consider sending a SIGTERM, then after so much time
    // if it still hasn't exited do a SIGKILL (can use a libprocess
    // process for this).
    LOG(INFO) << "Sending SIGKILL to gpid "
              << pgids[framework->frameworkId][executor->info.executor_id()];
    killpg(pgids[framework->frameworkId][executor->info.executor_id()], SIGKILL);
    pgids[framework->frameworkId][executor->info.executor_id()] = -1;
    executor->executorStatus = "No executor running";

    // TODO(benh): Kill all of the process's descendants? Perhaps
    // create a new libprocess process that continually tries to kill
    // all the processes that are a descendant of the executor, trying
    // to kill the executor last ... maybe this is just too much of a
    // burden?

    pgids[framework->frameworkId].erase(executor->info.executor_id());
  }
}


void ProcessBasedIsolationModule::resourcesChanged(Framework* framework, Executor* executor)
{
  // Do nothing; subclasses may override this.
}


ExecutorLauncher* ProcessBasedIsolationModule::createExecutorLauncher(Framework* framework, Executor* executor)
{
  // Create a map of parameters for the executor launcher.
  map<string, string> params;

  for (int i = 0; i < executor->info.params().param_size(); i++) {
    params[executor->info.params().param(i).key()] = 
      executor->info.params().param(i).value();
  }

  return 
    new ExecutorLauncher(framework->frameworkId,
                         executor->info.executor_id(),
                         executor->info.uri(),
                         framework->info.user(),
                         slave->getUniqueWorkDirectory(framework->frameworkId,
                                                       executor->info.executor_id()),
                         slave->self(),
                         slave->getConfiguration().get("frameworks_home", ""),
                         slave->getConfiguration().get("home", ""),
                         slave->getConfiguration().get("hadoop_home", ""),
                         !slave->local,
                         slave->getConfiguration().get("switch_user", true),
                         params);
}


ProcessBasedIsolationModule::Reaper::Reaper(ProcessBasedIsolationModule* m)
  : module(m)
{}


void ProcessBasedIsolationModule::Reaper::operator () ()
{
  link(module->slave->self());
  while (true) {
    receive(1);
    if (name() == TIMEOUT) {
      // Check whether any child process has exited.
      pid_t pid;
      int status;
      if ((pid = waitpid((pid_t) -1, &status, WNOHANG)) > 0) {
        foreachpair (const FrameworkID& frameworkId, _, module->pgids) {
          foreachpair (const ExecutorID& executorId, pid_t pgid, module->pgids[frameworkId]) {
            if (pgid == pid) {
              // Kill the process group to clean up the tasks.
              LOG(INFO) << "Sending SIGKILL to gpid " << pgid;
              killpg(pgid, SIGKILL);
              module->pgids[frameworkId][executorId] = -1;
              LOG(INFO) << "Telling slave of lost executor " << executorId
                        << " of framework " << frameworkId;
              // TODO(benh): This is broken if/when libprocess is parallel!
              module->slave->executorExited(frameworkId, executorId, status);
              module->pgids[frameworkId].erase(executorId);
              break;
            }
          }
        }
      }
    } else if (name() == TERMINATE || name() == EXIT) {
      return;
    }
  }
}
