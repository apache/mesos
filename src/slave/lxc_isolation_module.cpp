/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <sstream>
#include <map>

#include <process/dispatch.hpp>

#include "lxc_isolation_module.hpp"

#include "common/foreach.hpp"
#include "common/type_utils.hpp"
#include "common/units.hpp"
#include "common/utils.hpp"

#include "launcher/launcher.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using namespace process;

using launcher::ExecutorLauncher;

using process::wait; // Necessary on some OS's to disambiguate.

using std::map;
using std::max;
using std::string;
using std::vector;


namespace {

const int32_t CPU_SHARES_PER_CPU = 1024;
const int32_t MIN_CPU_SHARES = 10;
const int64_t MIN_MEMORY_MB = 128 * Megabyte;

} // namespace {


LxcIsolationModule::LxcIsolationModule()
  : initialized(false)
{
  // Spawn the reaper, note that it might send us a message before we
  // actually get spawned ourselves, but that's okay, the message will
  // just get dropped.
  reaper = new Reaper();
  spawn(reaper);
  dispatch(reaper, &Reaper::addProcessExitedListener, this);
}


LxcIsolationModule::~LxcIsolationModule()
{
  CHECK(reaper != NULL);
  terminate(reaper);
  wait(reaper);
  delete reaper;
}


void LxcIsolationModule::initialize(
    const Configuration& _conf,
    bool _local,
    const PID<Slave>& _slave)
{
  conf = _conf;
  local = _local;
  slave = _slave;

  // Check if Linux Container tools are available.
  if (system("lxc-version > /dev/null") != 0) {
    LOG(FATAL) << "Could not run lxc-version; make sure Linux Container "
                << "tools are installed";
  }

  // Check that we are root (it might also be possible to create Linux
  // containers without being root, but we can support that later).
  if (getuid() != 0) {
    LOG(FATAL) << "LXC isolation module requires slave to run as root";
  }

  initialized = true;
}


void LxcIsolationModule::launchExecutor(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Resources& resources)
{
  CHECK(initialized) << "Cannot launch executors before initialization!";

  const ExecutorID& executorId = executorInfo.executor_id();

  LOG(INFO) << "Launching " << executorId
            << " (" << executorInfo.uri() << ")"
            << " in " << directory
            << " with resources " << resources
            << "' for framework " << frameworkId;

  // Create a name for the container.
  std::ostringstream out;
  out << "mesos.executor-" << executorId << ".framework-" << frameworkId;

  const string& container = out.str();

  ContainerInfo* info = new ContainerInfo();

  info->frameworkId = frameworkId;
  info->executorId = executorId;
  info->container = container;
  info->pid = -1;

  infos[frameworkId][executorId] = info;

  // Run lxc-execute mesos-launcher using a fork-exec (since lxc-execute
  // does not return until the container is finished). Note that lxc-execute
  // automatically creates the container and will delete it when finished.
  pid_t pid;
  if ((pid = fork()) == -1) {
    PLOG(FATAL) << "Failed to fork to launch new executor";
  }

  if (pid) {
    // In parent process.
    LOG(INFO) << "Forked executor at = " << pid;

    // Record the pid.
    info->pid = pid;

    // Tell the slave this executor has started.
    dispatch(slave, &Slave::executorStarted,
             frameworkId, executorId, pid);
  } else {
    // Close unnecessary file descriptors. Note that we are assuming
    // stdin, stdout, and stderr can ONLY be found at the POSIX
    // specified file numbers (0, 1, 2).
    foreach (const string& entry, utils::os::listdir("/proc/self/fd")) {
      if (entry != "." && entry != "..") {
        try {
          int fd = boost::lexical_cast<int>(entry);
          if (fd != STDIN_FILENO &&
            fd != STDOUT_FILENO &&
            fd != STDERR_FILENO) {
            close(fd);
          }
        } catch (boost::bad_lexical_cast&) {
          LOG(FATAL) << "Failed to close file descriptors";
        }
      }
    }

    // Create an ExecutorLauncher to set up the environment for executing
    // an external launcher_main.cpp process (inside of lxc-execute).
    map<string, string> params;

    for (int i = 0; i < executorInfo.params().param_size(); i++) {
      params[executorInfo.params().param(i).key()] =
        executorInfo.params().param(i).value();
    }

    ExecutorLauncher* launcher =
      new ExecutorLauncher(frameworkId,
			   executorId,
			   executorInfo.uri(),
			   frameworkInfo.user(),
                           directory,
			   slave,
			   conf.get("frameworks_home", ""),
			   conf.get("home", ""),
			   conf.get("hadoop_home", ""),
			   !local,
			   conf.get("switch_user", true),
			   container,
			   params);

    launcher->setupEnvironmentForLauncherMain();

    // Construct the initial control group options that specify the
    // initial resources limits for this executor.
    const vector<string>& options = getControlGroupOptions(resources);

    const char** args = (const char**) new char*[3 + options.size() + 2];

    int i = 0;

    args[i++] = "lxc-execute";
    args[i++] = "-n";
    args[i++] = container.c_str();

    for (int j = 0; j < options.size(); j++) {
      args[i++] = options[j].c_str();
    }

    // Determine path for mesos-launcher from Mesos home directory.
    string path = conf.get("home", ".") + "/bin/mesos-launcher";
    args[i++] = path.c_str();
    args[i++] = NULL;

    // Run lxc-execute.
    execvp(args[0], (char* const*) args);

    // If we get here, the execvp call failed.
    LOG(FATAL) << "Could not exec lxc-execute";
  }
}


void LxcIsolationModule::killExecutor(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  CHECK(initialized) << "Cannot kill executors before initialization!";
  if (!infos.contains(frameworkId) ||
      !infos[frameworkId].contains(executorId)) {
    LOG(ERROR) << "ERROR! Asked to kill an unknown executor!";
    return;
  }

  ContainerInfo* info = infos[frameworkId][executorId];

  CHECK(info->container != "");

  LOG(INFO) << "Stopping container " << info->container;

  Try<int> status =
    utils::os::shell(NULL, "lxc-stop -n %s", info->container.c_str());

  if (status.isError()) {
    LOG(ERROR) << "Failed to stop container " << info->container
               << ": " << status.error();
  } else if (status.get() != 0) {
    LOG(ERROR) << "Failed to stop container " << info->container
               << ", lxc-stop returned: " << status.get();
  }

  if (infos[frameworkId].size() == 1) {
    infos.erase(frameworkId);
  } else {
    infos[frameworkId].erase(executorId);
  }

  delete info;

  // NOTE: Both frameworkId and executorId are no longer valid because
  // they have just been deleted above!
}


void LxcIsolationModule::resourcesChanged(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Resources& resources)
{
  CHECK(initialized) << "Cannot change resources before initialization!";
  if (!infos.contains(frameworkId) ||
      !infos[frameworkId].contains(executorId)) {
    LOG(ERROR) << "ERROR! Asked to update resources for an unknown executor!";
    return;
  }

  ContainerInfo* info = infos[frameworkId][executorId];

  CHECK(info->container != "");

  const string& container = info->container;

  // For now, just try setting the CPUs and memory right away, and kill the
  // framework if this fails (needs to be fixed).
  // A smarter thing to do might be to only update them periodically in a
  // separate thread, and to give frameworks some time to scale down their
  // memory usage.
  string property;
  uint64_t value;

  double cpu = resources.get("cpu", Resource::Scalar()).value();
  int32_t cpu_shares = max(CPU_SHARES_PER_CPU * (int32_t) cpu, MIN_CPU_SHARES);

  property = "cpu.shares";
  value = cpu_shares;

  if (!setControlGroupValue(container, property, value)) {
    // TODO(benh): Kill the executor, but do it in such a way that the
    // slave finds out about it exiting.
    return;
  }

  double mem = resources.get("mem", Resource::Scalar()).value();
  int64_t limit_in_bytes = max((int64_t) mem, MIN_MEMORY_MB) * 1024LL * 1024LL;

  property = "memory.limit_in_bytes";
  value = limit_in_bytes;

  if (!setControlGroupValue(container, property, value)) {
    // TODO(benh): Kill the executor, but do it in such a way that the
    // slave finds out about it exiting.
    return;
  }
}


void LxcIsolationModule::processExited(pid_t pid, int status)
{
  foreachkey (const FrameworkID& frameworkId, infos) {
    foreachvalue (ContainerInfo* info, infos[frameworkId]) {
      if (info->pid == pid) {
        LOG(INFO) << "Telling slave of lost executor "
		  << info->executorId
                  << " of framework " << info->frameworkId;

        dispatch(slave, &Slave::executorExited,
                 info->frameworkId, info->executorId, status);

        // Try and cleanup after the executor.
        killExecutor(info->frameworkId, info->executorId);
        return;
      }
    }
  }
}


bool LxcIsolationModule::setControlGroupValue(
    const string& container,
    const string& property,
    int64_t value)
{
  LOG(INFO) << "Setting " << property
            << " for container " << container
            << " to " << value;

  Try<int> status =
    utils::os::shell(NULL, "lxc-cgroup -n %s %s %lld",
                     container.c_str(), property.c_str(), value);

  if (status.isError()) {
    LOG(ERROR) << "Failed to set " << property
               << " for container " << container
               << ": " << status.error();
    return false;
  } else if (status.get() != 0) {
    LOG(ERROR) << "Failed to set " << property
               << " for container " << container
               << ": lxc-cgroup returned " << status.get();
    return false;
  }

  return true;
}


vector<string> LxcIsolationModule::getControlGroupOptions(
    const Resources& resources)
{
  vector<string> options;

  std::ostringstream out;

  double cpu = resources.get("cpu", Resource::Scalar()).value();
  int32_t cpu_shares = max(CPU_SHARES_PER_CPU * (int32_t) cpu, MIN_CPU_SHARES);

  options.push_back("-s");
  out << "lxc.cgroup.cpu.shares=" << cpu_shares;
  options.push_back(out.str());

  out.str("");

  double mem = resources.get("mem", Resource::Scalar()).value();
  int64_t limit_in_bytes = max((int64_t) mem, MIN_MEMORY_MB) * 1024LL * 1024LL;

  options.push_back("-s");
  out << "lxc.cgroup.memory.limit_in_bytes=" << limit_in_bytes;
  options.push_back(out.str());

  return options;
}
