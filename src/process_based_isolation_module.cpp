#ifdef __APPLE__
#define _XOPEN_SOURCE
#endif /* __APPLE__ */

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
  osPid[framework->id] = -1;
  framework->executorStatus = "No executor running";
}


void ProcessBasedIsolationModule::frameworkRemoved(Framework* framework)
{
  osPid.erase(framework->id);
}


void ProcessBasedIsolationModule::startExecutor(Framework* framework)
{
  LOG(INFO) << "Starting executor for framework " << framework->id << ": "
            << framework->executorInfo.uri;
  CHECK(osPid[framework->id] == -1);

  pid_t pid;
  if ((pid = fork()) == -1)
    PLOG(FATAL) << "Failed to fork to launch new executor";

  if (pid) {
    // In parent process
    LOG(INFO) << "Started executor, OS pid = " << pid;
    osPid[framework->id] = pid;
    framework->executorStatus = "PID: " + lexical_cast<string>(pid);
  } else {
    // In child process
    createExecutorLauncher(framework)->run();
  }
}


void ProcessBasedIsolationModule::killExecutor(Framework* fw)
{
  if (osPid[fw->id] != -1) {
    LOG(INFO) << "Sending SIGTERM to pid " << osPid[fw->id];
    kill(osPid[fw->id], SIGKILL);
    osPid[fw->id] = -1;
    fw->executorStatus = "No executor running";
    // TODO: Kill all of the process's descendants?
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
	foreachpair (FrameworkID fid, pid_t& fwPid, module->osPid) {
	  if (fwPid == pid) {
	    module->osPid[fid] = -1;
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

