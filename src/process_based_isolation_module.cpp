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

using namespace nexus;
using namespace nexus::internal;
using namespace nexus::internal::slave;


ProcessBasedIsolationModule::ProcessBasedIsolationModule(Slave* slave)
{
  this->slave = slave;
  reaper = new Reaper(this);
  Process::spawn(reaper);
}


ProcessBasedIsolationModule::~ProcessBasedIsolationModule()
{
  // We want to wait until the reaper has completed because it
  // accesses 'this' in order to make callbacks ... deleting 'this'
  // could thus lead to a seg fault!
  Process::post(reaper->getPID(), SHUTDOWN_REAPER);
  Process::wait(reaper);
  delete reaper;
}


void ProcessBasedIsolationModule::frameworkAdded(Framework* framework)
{
  pgids[framework->id] = -1;
  framework->executorStatus = "No executor running";
}


void ProcessBasedIsolationModule::frameworkRemoved(Framework* framework)
{
  pgids.erase(framework->id);
}


void ProcessBasedIsolationModule::startExecutor(Framework* framework)
{
  LOG(INFO) << "Starting executor for framework " << framework->id << ": "
            << framework->executorInfo.uri;
  CHECK(pgids[framework->id] == -1);

  pid_t pid;
  if ((pid = fork()) == -1)
    PLOG(FATAL) << "Failed to fork to launch new executor";

  if (pid) {
    // In parent process, record the gpid for killpg later (the pid is
    // the gpid because the child does a setsid below).
    LOG(INFO) << "Started executor, OS pid = " << pid;
    pgids[framework->id] = pid;
    framework->executorStatus = "PID: " + lexical_cast<string>(pid);
  } else {
    // In child process, do setsid to make cleanup easier.
    if ((pid = setsid()) == -1)
      perror("setsid error");

    createExecutorLauncher(framework)->run();
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
  }
}


void ProcessBasedIsolationModule::resourcesChanged(Framework* fw)
{
  // Do nothing; subclasses may override this.
}


ExecutorLauncher* ProcessBasedIsolationModule::createExecutorLauncher(Framework* fw)
{
  return new ExecutorLauncher(fw->id, fw->executorInfo.uri, fw->user,
			      slave->getWorkDirectory(fw->id),
			      slave->self(), !slave->local);
}


ProcessBasedIsolationModule::Reaper::Reaper(ProcessBasedIsolationModule* m)
  : module(m)
{}

  
void ProcessBasedIsolationModule::Reaper::operator () ()
{
  link(module->slave->getPID());
  while (true) {
    switch (receive(1)) {
    case PROCESS_TIMEOUT: {
      // Check whether any child process has exited
      pid_t pid;
      int status;
      if ((pid = waitpid((pid_t) -1, &status, WNOHANG)) > 0) {
	foreachpair (FrameworkID fid, pid_t& pgid, module->pgids) {
	  if (pgid == pid) {
	    // Kill the process group to clean up the tasks.
	    LOG(INFO) << "Sending SIGKILL to gpid " << pgid;
	    killpg(pgid, SIGKILL);
	    module->pgids[fid] = -1;
	    LOG(INFO) << "Telling slave of lost framework " << fid;
	    // TODO(benh): This is broken if/when libprocess is parallel!
	    module->slave->executorExited(fid, status);
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

