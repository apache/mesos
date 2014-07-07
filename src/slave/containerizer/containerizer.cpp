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

#include <map>
#include <vector>

#include <process/dispatch.hpp>
#include <process/owned.hpp>

#include <stout/fs.hpp>
#include <stout/hashmap.hpp>
#include <stout/net.hpp>
#include <stout/stringify.hpp>
#include <stout/uuid.hpp>

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#ifdef __linux__
#include "slave/containerizer/linux_launcher.hpp"
#endif // __linux__
#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/isolator.hpp"
#include "slave/containerizer/launcher.hpp"
#include "slave/containerizer/external_containerizer.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

using std::map;
using std::string;
using std::vector;

using namespace process;

namespace mesos {
namespace internal {
namespace slave {

// TODO(idownes): Move this to the Containerizer interface to complete
// the delegation of containerization, i.e., external containerizers should be
// able to report the resources they can isolate.
Try<Resources> Containerizer::resources(const Flags& flags)
{
  Try<Resources> parsed = Resources::parse(
      flags.resources.get(""), flags.default_role);

  if (parsed.isError()) {
    return Error(parsed.error());
  }

  Resources resources = parsed.get();

  // CPU resource.
  if (!resources.cpus().isSome()) {
    // No CPU specified so probe OS or resort to DEFAULT_CPUS.
    double cpus;
    Try<long> cpus_ = os::cpus();
    if (!cpus_.isSome()) {
      LOG(WARNING) << "Failed to auto-detect the number of cpus to use: '"
                   << cpus_.error()
                   << "'; defaulting to " << DEFAULT_CPUS;
      cpus = DEFAULT_CPUS;
    } else {
      cpus = cpus_.get();
    }

    resources += Resources::parse(
        "cpus",
        stringify(cpus),
        flags.default_role).get();
  }

  // Memory resource.
  if (!resources.mem().isSome()) {
    // No memory specified so probe OS or resort to DEFAULT_MEM.
    Bytes mem;
    Try<os::Memory> mem_ = os::memory();
    if (mem_.isError()) {
      LOG(WARNING) << "Failed to auto-detect the size of main memory: '"
                    << mem_.error()
                    << "' ; defaulting to DEFAULT_MEM";
      mem = DEFAULT_MEM;
    } else {
      Bytes total = mem_.get().total;
      if (total >= Gigabytes(2)) {
        mem = total - Gigabytes(1); // Leave 1GB free.
      } else {
        mem = Bytes(total.bytes() / 2); // Use 50% of the memory.
      }
    }

    resources += Resources::parse(
        "mem",
        stringify(mem.megabytes()),
        flags.default_role).get();
  }

  // Disk resource.
  if (!resources.disk().isSome()) {
    // No disk specified so probe OS or resort to DEFAULT_DISK.
    Bytes disk;

    // NOTE: We calculate disk size of the file system on
    // which the slave work directory is mounted.
    Try<Bytes> disk_ = fs::size(flags.work_dir);
    if (!disk_.isSome()) {
      LOG(WARNING) << "Failed to auto-detect the disk space: '"
                   << disk_.error()
                   << "' ; defaulting to " << DEFAULT_DISK;
      disk = DEFAULT_DISK;
    } else {
      Bytes total = disk_.get();
      if (total >= Gigabytes(10)) {
        disk = total - Gigabytes(5); // Leave 5GB free.
      } else {
        disk = Bytes(total.bytes() / 2); // Use 50% of the disk.
      }
    }

    resources += Resources::parse(
        "disk",
        stringify(disk.megabytes()),
        flags.default_role).get();
  }

  // Network resource.
  if (!resources.ports().isSome()) {
    // No ports specified so resort to DEFAULT_PORTS.
    resources += Resources::parse(
        "ports",
        stringify(DEFAULT_PORTS),
        flags.default_role).get();
  }

  return resources;
}


Try<Containerizer*> Containerizer::create(const Flags& flags, bool local)
{
  if (flags.isolation == "external") {
    return new ExternalContainerizer(flags);
  }

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, local);

  if (containerizer.isError()) {
    return Error(containerizer.error());
  }

  return containerizer.get();
}


map<string, string> executorEnvironment(
    const ExecutorInfo& executorInfo,
    const string& directory,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint,
    const Duration& recoveryTimeout)
{
  map<string, string> env;
  // Set LIBPROCESS_PORT so that we bind to a random free port (since
  // this might have been set via --port option). We do this before
  // the environment variables below in case it is included.
  env["LIBPROCESS_PORT"] = "0";

  // Also add MESOS_NATIVE_JAVA_LIBRARY if it's not already present (and
  // like above, we do this before the environment variables below in
  // case the framework wants to override).
  // TODO(tillt): Adapt library towards JNI specific name once libmesos
  // has been split.
  if (!os::hasenv("MESOS_NATIVE_JAVA_LIBRARY")) {
    string path =
#ifdef __APPLE__
      LIBDIR "/libmesos-" VERSION ".dylib";
#else
      LIBDIR "/libmesos-" VERSION ".so";
#endif
    if (os::exists(path)) {
      env["MESOS_NATIVE_JAVA_LIBRARY"] = path;
    }
  }

  // Also add MESOS_NATIVE_LIBRARY if it's not already present.
  // This environment variable is kept for offering non JVM-based
  // frameworks a more compact and JNI independent library.
  if (!os::hasenv("MESOS_NATIVE_LIBRARY")) {
    string path =
#ifdef __APPLE__
      LIBDIR "/libmesos-" VERSION ".dylib";
#else
      LIBDIR "/libmesos-" VERSION ".so";
#endif
    if (os::exists(path)) {
      env["MESOS_NATIVE_LIBRARY"] = path;
    }
  }

  env["MESOS_FRAMEWORK_ID"] = executorInfo.framework_id().value();
  env["MESOS_EXECUTOR_ID"] = executorInfo.executor_id().value();
  env["MESOS_DIRECTORY"] = directory;
  env["MESOS_SLAVE_ID"] = slaveId.value();
  env["MESOS_SLAVE_PID"] = stringify(slavePid);
  env["MESOS_CHECKPOINT"] = checkpoint ? "1" : "0";

  if (checkpoint) {
    env["MESOS_RECOVERY_TIMEOUT"] = stringify(recoveryTimeout);
  }

  return env;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
