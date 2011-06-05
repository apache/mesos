#include "lxc_isolation_module.hpp"

#include <algorithm>

#include "foreach.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::list;
using std::make_pair;
using std::max;
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


LxcIsolationModule::LxcIsolationModule(Slave* slave)
{
  this->slave = slave;
  reaper = new Reaper(this);
  Process::spawn(reaper);
}


LxcIsolationModule::~LxcIsolationModule()
{
  // We want to wait until the reaper has completed because it
  // accesses 'this' in order to make callbacks ... deleting 'this'
  // could thus lead to a seg fault!
  Process::post(reaper->getPID(), SHUTDOWN_REAPER);
  Process::wait(reaper);
  delete reaper;
}


void LxcIsolationModule::frameworkAdded(Framework* framework)
{
  lxcExecutePid[framework->id] = -1;
  container[framework->id] = "";
  framework->executorStatus = "No executor running";
}


void LxcIsolationModule::frameworkRemoved(Framework *framework)
{
  lxcExecutePid.erase(framework->id);
  container.erase(framework->id);
}


void LxcIsolationModule::startExecutor(Framework *fw)
{
  LOG(INFO) << "Starting executor for framework " << fw->id << ": "
            << fw->executorInfo.uri;
  CHECK(lxcExecutePid[fw->id] == -1 && container[fw->id] == "");

  // Get location of Nexus install in order to find nexus-launcher.
  const char *nexusHome = getenv("NEXUS_HOME");
  if (!nexusHome)
    nexusHome = "..";
  string nexusLauncher = string(nexusHome) + "/src/nexus-launcher";

  // Create a name for the container
  ostringstream oss;
  oss << "nexus.slave-" << slave->id << ".framework-" << fw->id;
  string containerName = oss.str();

  container[fw->id] = containerName;
  fw->executorStatus = "Container: " + containerName;

  // Run lxc-execute nexus-launcher using a fork-exec (since lxc-execute
  // does not return until the container is finished). Note that lxc-execute
  // automatically creates the container and will delete it when finished.
  pid_t pid;
  if ((pid = fork()) == -1)
    PLOG(FATAL) << "Failed to fork to launch lxc-execute";

  if (pid) {
    // In parent process
    lxcExecutePid[fw->id] = pid;
    LOG(INFO) << "Started lxc-execute, pid = " << pid;
    int status;
  } else {
    // Set up environment variables.
    setenv("NEXUS_FRAMEWORK_ID", lexical_cast<string>(fw->id).c_str(), 1);
    setenv("NEXUS_EXECUTOR_URI", fw->executorInfo.uri.c_str(), 1);
    setenv("NEXUS_USER", fw->user.c_str(), 1);
    setenv("NEXUS_SLAVE_PID", lexical_cast<string>(slave->self()).c_str(), 1);
    setenv("NEXUS_REDIRECT_IO", slave->local ? "1" : "0", 1);
    setenv("NEXUS_WORK_DIRECTORY", slave->getWorkDirectory(fw->id).c_str(), 1);

    // Run lxc-execute.
    execlp("lxc-execute", "lxc-execute", "-n", containerName.c_str(),
           nexusLauncher.c_str(), (char *) NULL);
    // If we get here, the execl call failed.
    fatalerror("Could not exec lxc-execute");
    // TODO: Exit the slave if this happens
  }
}


void LxcIsolationModule::killExecutor(Framework* fw)
{
  if (container[fw->id] != "") {
    LOG(INFO) << "Stopping container " << container[fw->id];
    int ret = shell("lxc-stop -n %s", container[fw->id].c_str());
    if (ret != 0)
      LOG(ERROR) << "lxc-stop returned " << ret;
    container[fw->id] = "";
    fw->executorStatus = "No executor running";
  }
}


void LxcIsolationModule::resourcesChanged(Framework* fw)
{
  if (container[fw->id] != "") {
    // For now, just try setting the CPUs and mem right away.
    // A slightly smarter thing might be to only update them periodically.
    int ret;
    
    int32_t cpuShares = max(1024 * fw->resources.cpus, 10);
    LOG(INFO) << "Setting CPU shares for " << fw->id << " to " << cpuShares;
    ret = shell("lxc-cgroup -n %s cpu.shares %d",
                container[fw->id].c_str(), cpuShares);
    if (ret != 0)
      LOG(ERROR) << "lxc-cgroup returned " << ret;

    int64_t rssLimit = max(fw->resources.mem, 128 * Megabyte);
    LOG(INFO) << "Setting RSS limit for " << fw->id << " to " << rssLimit;
    ret = shell("lxc-cgroup -n %s memory.limit_in_bytes %lld",
                container[fw->id].c_str(), rssLimit);
    if (ret != 0)
      LOG(ERROR) << "lxc-cgroup returned " << ret;
    
    // TODO: Decreasing the RSS limit will fail if the current RSS is too
    // large and memory can't be swapped out. In that case, we should
    // either freeze the container before changing RSS, or just kill it.
  }
}


int LxcIsolationModule::shell(const char* fmt, ...)
{
  char *cmd;
  FILE *f;
  int ret;
  va_list args;
  va_start(args, fmt);
  if (vasprintf(&cmd, fmt, args) == -1)
    return -1;
  if ((f = popen(cmd, "w")) == NULL)
    return -1;
  ret = pclose(f);
  if (ret == -1)
    LOG(INFO) << "pclose error: " << strerror(errno);
  free(cmd);
  va_end(args);
  return ret;
}


LxcIsolationModule::Reaper::Reaper(LxcIsolationModule* m)
  : module(m)
{}

  
void LxcIsolationModule::Reaper::operator () ()
{
  link(module->slave->getPID());
  while (true) {
    switch (receive(1)) {
    case PROCESS_TIMEOUT: {
      // Check whether any child process has exited
      pid_t pid;
      int status;
      if ((pid = waitpid((pid_t) -1, &status, WNOHANG)) > 0) {
	foreachpair (FrameworkID fid, pid_t& fwPid, module->lxcExecutePid) {
	  if (fwPid == pid) {
	    module->container[fid] = "";
	    module->lxcExecutePid[fid] = -1;
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
