// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <map>
#include <set>
#include <vector>

#include <mesos/secret/resolver.hpp>

#include <process/dispatch.hpp>
#include <process/owned.hpp>

#include <stout/fs.hpp>
#include <stout/hashmap.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include "hook/manager.hpp"

#include "slave/csi_server.hpp"
#include "slave/flags.hpp"
#include "slave/gc.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/composing.hpp"
#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/docker.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launcher.hpp"
#ifdef __linux__
#include "slave/containerizer/mesos/linux_launcher.hpp"
#endif // __linux__

#include "slave/containerizer/mesos/isolators/gpu/nvidia.hpp"

using std::map;
using std::set;
using std::string;
using std::vector;

using namespace process;

namespace mesos {
namespace internal {
namespace slave {

// TODO(idownes): Move this to the Containerizer interface to complete
// the delegation of containerization.
Try<Resources> Containerizer::resources(const Flags& flags)
{
  Try<Resources> parsed = Resources::parse(
      flags.resources.getOrElse(""), flags.default_role);

  if (parsed.isError()) {
    return Error(parsed.error());
  }

  Resources resources = parsed.get();

  // NOTE: We need to check for the "cpus" resource within the flags
  // because once the `Resources` object is created, we cannot distinguish
  // between
  //  (1) "cpus:0", and
  //  (2) no cpus specified.
  // due to `Resources:add` discarding empty resources.
  // We only auto-detect cpus in case (2).
  // The same logic applies for the other resources!
  // `Resources::fromString().get()` is safe because `Resources::parse()` above
  // is valid.
  vector<Resource> resourceList = Resources::fromString(
      flags.resources.getOrElse(""), flags.default_role).get();

  bool hasCpus = false;
  bool hasMem = false;
  bool hasDisk = false;
  bool hasPorts = false;

  foreach (const Resource& resource, resourceList) {
    if (resource.name() == "cpus") {
      hasCpus = true;
    } else if (resource.name() == "mem") {
      hasMem = true;
    } else if (resource.name() == "disk") {
      hasDisk = true;
    } else if (resource.name() == "ports") {
      hasPorts = true;
    }
  }

  if (!hasCpus) {
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

#ifdef __linux__
  // GPU resource.
  Try<Resources> gpus = NvidiaGpuAllocator::resources(flags);
  if (gpus.isError()) {
    return Error("Failed to obtain GPU resources: " + gpus.error());
  }

  // When adding in the GPU resources, make sure that we filter out
  // the existing GPU resources (if any) so that we do not double
  // allocate GPUs.
  resources = gpus.get() + resources.filter(
      [](const Resource& resource) {
        return resource.name() != "gpus";
      });
#endif

  // Memory resource.
  if (!hasMem) {
    // No memory specified so probe OS or resort to DEFAULT_MEM.
    Bytes mem;
    Try<os::Memory> mem_ = os::memory();
    if (mem_.isError()) {
      LOG(WARNING) << "Failed to auto-detect the size of main memory: '"
                    << mem_.error()
                    << "' ; defaulting to DEFAULT_MEM";
      mem = DEFAULT_MEM;
    } else {
      Bytes total = mem_->total;
      if (total >= Gigabytes(2)) {
        mem = total - Gigabytes(1); // Leave 1GB free.
      } else {
        mem = Bytes(total.bytes() / 2); // Use 50% of the memory.
      }
    }

    // NOTE: The size is truncated here to preserve the existing
    // behavior for backward compatibility.
    resources += Resources::parse(
        "mem",
        stringify(mem.bytes() / Bytes::MEGABYTES),
        flags.default_role).get();
  }

  // Disk resource.
  if (!hasDisk) {
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

    // NOTE: The size is truncated here to preserve the existing
    // behavior for backward compatibility.
    resources += Resources::parse(
        "disk",
        stringify(disk.bytes() / Bytes::MEGABYTES),
        flags.default_role).get();
  }

  // Network resource.
  if (!hasPorts) {
    // No ports specified so resort to DEFAULT_PORTS.
    resources += Resources::parse(
        "ports",
        stringify(DEFAULT_PORTS),
        flags.default_role).get();
  }

  Option<Error> error = Resources::validate(resources);
  if (error.isSome()) {
    return error.get();
  }

  return resources;
}


Try<Containerizer*> Containerizer::create(
    const Flags& flags,
    bool local,
    Fetcher* fetcher,
    GarbageCollector* gc,
    SecretResolver* secretResolver,
    VolumeGidManager* volumeGidManager,
    PendingFutureTracker* futureTracker,
    CSIServer* csiServer)
{
  // Get the set of containerizer types.
  const vector<string> _types = strings::split(flags.containerizers, ",");
  const set<string> containerizerTypes(_types.begin(), _types.end());

  if (containerizerTypes.size() != _types.size()) {
    return Error("Duplicate entries found in --containerizer flag"
                 " '" + flags.containerizers + "'");
  }

  // Optionally create the Nvidia components.
  Option<NvidiaComponents> nvidia;

#ifdef __linux__
  if (nvml::isAvailable()) {
    // If we are using the docker containerizer (either alone or in
    // conjunction with the mesos containerizer), unconditionally
    // create the Nvidia components and pass them through. If we are
    // using the mesos containerizer alone, make sure we also have the
    // `gpu/nvidia` isolator flag set before creating these components.
    bool shouldCreate = false;

    if (containerizerTypes.count("docker") > 0) {
      shouldCreate = true;
    } else if (containerizerTypes.count("mesos") > 0) {
      const vector<string> _isolators = strings::tokenize(flags.isolation, ",");
      const set<string> isolators(_isolators.begin(), _isolators.end());

      if (isolators.count("gpu/nvidia") > 0) {
        shouldCreate = true;
      }
    }

    if (shouldCreate) {
      Try<Resources> gpus = NvidiaGpuAllocator::resources(flags);

      if (gpus.isError()) {
        return Error("Failed call to NvidiaGpuAllocator::resources: " +
                     gpus.error());
      }

      Try<NvidiaGpuAllocator> allocator =
        NvidiaGpuAllocator::create(flags, gpus.get());

      if (allocator.isError()) {
        return Error("Failed to NvidiaGpuAllocator::create: " +
                     allocator.error());
      }

      Try<NvidiaVolume> volume = NvidiaVolume::create();

      if (volume.isError()) {
        return Error("Failed to NvidiaVolume::create: " + volume.error());
      }

      nvidia = NvidiaComponents(allocator.get(), volume.get());
    }
  }
#endif

  // TODO(benh): We need to store which containerizer or
  // containerizers were being used. See MESOS-1663.

  // Create containerizer(s).
  vector<Containerizer*> containerizers;

  foreach (const string& type, containerizerTypes) {
    if (type == "mesos") {
      Try<MesosContainerizer*> containerizer = MesosContainerizer::create(
          flags,
          local,
          fetcher,
          gc,
          secretResolver,
          nvidia,
          volumeGidManager,
          futureTracker,
          csiServer);

      if (containerizer.isError()) {
        return Error("Could not create MesosContainerizer: " +
                     containerizer.error());
      } else {
        containerizers.push_back(containerizer.get());
      }
    } else if (type == "docker") {
      Try<DockerContainerizer*> containerizer =
        DockerContainerizer::create(flags, fetcher, nvidia);
      if (containerizer.isError()) {
        return Error("Could not create DockerContainerizer: " +
                     containerizer.error());
      } else {
        containerizers.push_back(containerizer.get());
      }
    } else {
      return Error("Unknown or unsupported containerizer: " + type);
    }
  }

  if (containerizers.size() == 1) {
    return containerizers.front();
  }

  Try<ComposingContainerizer*> containerizer =
    ComposingContainerizer::create(containerizers);

  if (containerizer.isError()) {
    return Error(containerizer.error());
  }

  return containerizer.get();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
