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

#ifndef __WINDOWS__
#include <fts.h>
#endif // __WINDOWS__

#include <mesos/type_utils.hpp>

#ifndef __WINDOWS__
#include <mesos/docker/spec.hpp>
#endif // __WINDOWS__

#include <mesos/secret/resolver.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/uuid.hpp>

#include <stout/os/realpath.hpp>

#ifdef __linux__
#include "linux/fs.hpp"
#endif

#include "slave/paths.hpp"
#include "slave/state.hpp"

#include "slave/containerizer/mesos/provisioner/constants.hpp"
#include "slave/containerizer/mesos/provisioner/backend.hpp"
#include "slave/containerizer/mesos/provisioner/paths.hpp"
#include "slave/containerizer/mesos/provisioner/provisioner.hpp"
#include "slave/containerizer/mesos/provisioner/store.hpp"

using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Promise;
using process::ReadWriteLock;

using mesos::internal::slave::AUFS_BACKEND;
using mesos::internal::slave::BIND_BACKEND;
using mesos::internal::slave::COPY_BACKEND;
using mesos::internal::slave::OVERLAY_BACKEND;

using mesos::slave::ContainerLayers;
using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {

// Validate whether the backend is supported on the underlying
// filesystem. Please see the following logic table for detail:
// +---------+--------------+------------------------------------------+
// | Backend | Suggested on | Disabled on                              |
// +---------+--------------+------------------------------------------+
// | aufs    | ext4 xfs     | btrfs aufs eCryptfs                      |
// | overlay | ext4 xfs     | btrfs aufs overlay overlay2 zfs eCryptfs |
// | bind    |              | N/A(`--sandbox_directory' must exist)    |
// | copy    |              | N/A                                      |
// +---------+--------------+------------------------------------------+
static Try<Nothing> validateBackend(
    const string& backend,
    const string& directory)
{
  // Copy backend is supported on all underlying filesystems.
  if (backend == COPY_BACKEND) {
    return Nothing();
  }

#ifdef __linux__
  // Bind backend is supported on all underlying filesystems.
  if (backend == BIND_BACKEND) {
    return Nothing();
  }

  Try<uint32_t> fsType = fs::type(directory);
  if (fsType.isError()) {
    return Error(
      "Failed to get filesystem type id from directory '" +
      directory + "': " + fsType.error());
  }

  Try<string> _fsTypeName = fs::typeName(fsType.get());

  string fsTypeName = _fsTypeName.isSome()
    ? _fsTypeName.get()
    : stringify(fsType.get());

  if (backend == OVERLAY_BACKEND) {
    vector<uint32_t> exclusives = {
      FS_TYPE_AUFS,
      FS_TYPE_BTRFS,
      FS_TYPE_ECRYPTFS,
      FS_TYPE_ZFS,
      FS_TYPE_OVERLAY
    };

    if (std::find(exclusives.begin(),
                  exclusives.end(),
                  fsType.get()) != exclusives.end()) {
      return Error(
          "Backend '" + stringify(OVERLAY_BACKEND) + "' is not supported "
          "on the underlying filesystem '" + fsTypeName + "'");
    }

    // Check whether `d_type` is supported. We need to create a
    // directory entry to probe this since `.` and `..` may be
    // marked `DT_DIR` even if the filesystem does not otherwise
    // have `d_type` support.
    string probeDir = path::join(directory, ".probe");

    Try<Nothing> mkdir = os::mkdir(probeDir);
    if (mkdir.isError()) {
      return Error(
          "Failed to create temporary directory '" +
          probeDir + "': " + mkdir.error());
    }

    Try<bool> supportDType = fs::dtypeSupported(directory);

    // clean up first
    Try<Nothing> rmdir = os::rmdir(probeDir);
    if (rmdir.isError()) {
      LOG(WARNING) << "Failed to remove temporary directory"
                   << "' " << probeDir << "': " << rmdir.error();
     }

    if (supportDType.isError()) {
      return Error(
          "Cannot verify filesystem attributes: " +
          supportDType.error());
    }

    if (!supportDType.get()) {
      return Error(
          "Backend '" + stringify(OVERLAY_BACKEND) +
          "' is not supported due to missing d_type support " +
          "on the underlying filesystem");
    }

    return Nothing();
  }

  if (backend == AUFS_BACKEND) {
    vector<uint32_t> exclusives = {
      FS_TYPE_AUFS,
      FS_TYPE_BTRFS,
      FS_TYPE_ECRYPTFS
    };

    if (std::find(exclusives.begin(),
                  exclusives.end(),
                  fsType.get()) != exclusives.end()) {
      return Error(
          "Backend '" + stringify(AUFS_BACKEND) + "' is not supported "
          "on the underlying filesystem '" + fsTypeName + "'");
    }

    return Nothing();
  }
#endif // __linux__

  return Error("Validation not supported");
}


Try<Owned<Provisioner>> Provisioner::create(
    const Flags& flags,
    SecretResolver* secretResolver)
{
  const string _rootDir = slave::paths::getProvisionerDir(flags.work_dir);

  Try<Nothing> mkdir = os::mkdir(_rootDir);
  if (mkdir.isError()) {
    return Error(
        "Failed to create provisioner root directory '" +
        _rootDir + "': " + mkdir.error());
  }

  Result<string> rootDir = os::realpath(_rootDir);
  if (rootDir.isError()) {
    return Error(
        "Failed to resolve the realpath of provisioner root directory '" +
        _rootDir + "': " + rootDir.error());
  }

  CHECK_SOME(rootDir); // Can't be None since we just created it.

  hashmap<string, Owned<Backend>> backends = Backend::create(flags);
  if (backends.empty()) {
    return Error("No usable provisioner backend created");
  }

  // Determine the default backend:
  // 1) If the user specifies the backend, make sure it is supported
  //    w.r.t. the underlying filesystem.
  // 2) If the user does not specify the backend, pick the default
  //    backend according to a pre-defined order, and make sure the
  //    picked one is supported w.r.t. the underlying filesystem.
  //
  // TODO(jieyu): Only validating backends against provisioner dir is
  // not sufficient. We need to validate against all the store dir as
  // well. Consider introducing a default backend for each store.
  Option<string> defaultBackend;

  if (flags.image_provisioner_backend.isSome()) {
    if (!backends.contains(flags.image_provisioner_backend.get())) {
      return Error(
          "The specified provisioner backend '" +
          flags.image_provisioner_backend.get() +
          "' is not supported: Not found");
    }

    Try<Nothing> supported = validateBackend(
        flags.image_provisioner_backend.get(),
        rootDir.get());

    if (supported.isError()) {
      return Error(
          "The specified provisioner backend '" +
          flags.image_provisioner_backend.get() +
          "' is not supported: " + supported.error());
    }

    defaultBackend = flags.image_provisioner_backend.get();
  } else {
    // TODO(gilbert): Consider select the bind backend if it is a
    // single layer image. Please note that a read-only filesystem
    // (e.g., using the bind backend) requires the sandbox already
    // exists.
    //
    // Choose a backend smartly if no backend is specified. The follow
    // list is a priority list, meaning that we favor backends in the
    // front of the list.
    vector<string> backendNames = {
#ifdef __linux__
      OVERLAY_BACKEND,
      AUFS_BACKEND,
#endif // __linux__
      COPY_BACKEND
    };

    foreach (const string& backendName, backendNames) {
      if (!backends.contains(backendName)) {
        continue;
      }

      Try<Nothing> supported = validateBackend(backendName, rootDir.get());
      if (supported.isError()) {
        LOG(INFO) << "Provisioner backend '"
                  << backendName << "' is not supported on '"
                  << rootDir.get() << "': " << supported.error();
        continue;
      }

      defaultBackend = backendName;
      break;
    }

    if (defaultBackend.isNone()) {
      return Error("Failed to find a default backend");
    }
  }

  CHECK_SOME(defaultBackend);

  LOG(INFO) << "Using default backend '" << defaultBackend.get() << "'";

  return Provisioner::create(
      flags,
      rootDir.get(),
      defaultBackend.get(),
      backends,
      secretResolver);
}


Try<Owned<Provisioner>> Provisioner::create(
    const Flags& flags,
    const string& rootDir,
    const string& defaultBackend,
    const hashmap<string, Owned<Backend>>& backends,
    SecretResolver* secretResolver)
{
  Try<hashmap<Image::Type, Owned<Store>>> stores =
    Store::create(flags, secretResolver);

  if (stores.isError()) {
    return Error("Failed to create image stores: " + stores.error());
  }

  return Owned<Provisioner>(new Provisioner(
      Owned<ProvisionerProcess>(new ProvisionerProcess(
          rootDir,
          defaultBackend,
          stores.get(),
          backends))));
}


Provisioner::Provisioner(Owned<ProvisionerProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Provisioner::~Provisioner()
{
  if (process.get() != nullptr) {
    terminate(process.get());
    wait(process.get());
  }
}


Future<Nothing> Provisioner::recover(
    const hashset<ContainerID>& knownContainerIds) const
{
  return dispatch(
      CHECK_NOTNULL(process.get()),
      &ProvisionerProcess::recover,
      knownContainerIds);
}


Future<ProvisionInfo> Provisioner::provision(
    const ContainerID& containerId,
    const Image& image) const
{
  return dispatch(
      CHECK_NOTNULL(process.get()),
      &ProvisionerProcess::provision,
      containerId,
      image);
}


Future<bool> Provisioner::destroy(const ContainerID& containerId) const
{
  return dispatch(
      CHECK_NOTNULL(process.get()),
      &ProvisionerProcess::destroy,
      containerId);
}


Future<Nothing> Provisioner::pruneImages(
    const vector<Image>& excludedImages) const
{
  return dispatch(
      CHECK_NOTNULL(process.get()),
      &ProvisionerProcess::pruneImages,
      excludedImages);
}


ProvisionerProcess::ProvisionerProcess(
    const string& _rootDir,
    const string& _defaultBackend,
    const hashmap<Image::Type, Owned<Store>>& _stores,
    const hashmap<string, Owned<Backend>>& _backends)
  : ProcessBase(process::ID::generate("mesos-provisioner")),
    rootDir(_rootDir),
    defaultBackend(_defaultBackend),
    stores(_stores),
    backends(_backends) {}


Future<Nothing> ProvisionerProcess::recover(
    const hashset<ContainerID>& knownContainerIds)
{
  // List provisioned containers, recover known ones, and destroy
  // unknown ones. Note that known orphan containers are recovered as
  // well and they will be destroyed by the containerizer using the
  // normal cleanup path. See MESOS-2367 for details.
  //
  // NOTE: All containers, including top level container and child
  // containers, will be included in the hashset.
  Try<hashset<ContainerID>> containers =
    provisioner::paths::listContainers(rootDir);

  if (containers.isError()) {
    return Failure(
        "Failed to list the containers managed by the provisioner: " +
        containers.error());
  }

  // Scan the list of containers, register all of them with 'infos'
  // but mark unknown containers for immediate cleanup.
  hashset<ContainerID> unknownContainerIds;

  foreach (const ContainerID& containerId, containers.get()) {
    Owned<Info> info = Owned<Info>(new Info());
    info->provisioning = ProvisionInfo{};

    Try<hashmap<string, hashset<string>>> rootfses =
      provisioner::paths::listContainerRootfses(rootDir, containerId);

    if (rootfses.isError()) {
      return Failure(
          "Unable to list rootfses belonged to container " +
          stringify(containerId) + ": " + rootfses.error());
    }

    foreachkey (const string& backend, rootfses.get()) {
      if (!backends.contains(backend)) {
        return Failure(
            "Found rootfses managed by an unrecognized backend: " + backend);
      }

      info->rootfses.put(backend, rootfses.get()[backend]);
    }

    const string path = provisioner::paths::getLayersFilePath(
      rootDir, containerId);

    if (!os::exists(path)) {
      // This is possible if we recovered a container provisioned before we
      // started to checkpoint `ContainerLayers`.
      VLOG(1) << "Layers path '" << path << "' is missing for container' "
              << containerId << "'";
    } else {
      Result<ContainerLayers> layers = state::read<ContainerLayers>(path);
      if (layers.isError()) {
        return Failure(
            "Failed to recover layers for container '" +
            stringify(containerId) + "': " + layers.error());
      }

      if (layers.isSome()) {
        info->layers = vector<string>();
        std::copy(
            layers->paths().begin(),
            layers->paths().end(),
            std::back_inserter(info->layers.get()));

        if (layers->has_config()) {
          info->layers->push_back(layers->config());
        }
      }
    }

    infos.put(containerId, info);

    if (knownContainerIds.contains(containerId)) {
      LOG(INFO) << "Recovered container " << containerId;
      continue;
    } else {
      // For immediate cleanup below.
      unknownContainerIds.insert(containerId);
    }
  }

  // Cleanup unknown orphan containers' rootfses.
  vector<Future<bool>> cleanups;
  foreach (const ContainerID& containerId, unknownContainerIds) {
    LOG(INFO) << "Cleaning up unknown container " << containerId;

    // If a container is unknown, it means the launcher has not forked
    // it yet. So an unknown container should not have any child. It
    // means that when destroying an unknown container, we can just
    // simply call 'destroy' directly, without needing to make a
    // recursive call to destroy.
    cleanups.push_back(destroy(containerId));
  }

  Future<Nothing> cleanup = collect(cleanups)
    .then([]() -> Future<Nothing> { return Nothing(); });

  // Recover stores.
  vector<Future<Nothing>> recovers;
  foreachvalue (const Owned<Store>& store, stores) {
    recovers.push_back(store->recover());
  }

  Future<Nothing> recover = collect(recovers)
    .then([]() -> Future<Nothing> { return Nothing(); });

  // A successful provisioner recovery depends on:
  //  1) Recovery of known containers (done above).
  //  2) Successful cleanup of unknown containers.
  //  3) Successful store recovery.
  //
  // TODO(jieyu): Do not recover 'store' before unknown containers are
  // cleaned up. In the future, we may want to cleanup unused rootfses
  // in 'store', which might fail if there still exist unknown
  // containers holding references to them.
  return collect(cleanup, recover)
    .then([=]() -> Future<Nothing> {
      LOG(INFO) << "Provisioner recovery complete";
      return Nothing();
    });
}


Future<ProvisionInfo> ProvisionerProcess::provision(
    const ContainerID& containerId,
    const Image& image)
{
  // `destroy` and `provision` can happen concurrently, but `pruneImages`
  // is exclusive.
  return rwLock.read_lock()
    .then(defer(self(), [this, containerId, image]() -> Future<ProvisionInfo> {
      if (!stores.contains(image.type())) {
        return Failure(
            "Unsupported container image type: " + stringify(image.type()));
      }

      Owned<Promise<ProvisionInfo>> promise(new Promise<ProvisionInfo>());

      // Get and then provision image layers from the store.
      Future<ProvisionInfo> future =
        stores.at(image.type())->get(image, defaultBackend)
          .then(defer(
              self(),
              &Self::_provision,
              containerId,
              image,
              defaultBackend,
              lambda::_1))
          .onAny(defer(self(), [=](const Future<ProvisionInfo>& provisionInfo) {
            CHECK(!provisionInfo.isPending());

            if (provisionInfo.isReady()) {
              promise->set(provisionInfo);
            } else if (provisionInfo.isDiscarded()) {
              promise->discard();
            } else {
              promise->fail(provisionInfo.failure());
            }
          }));

      return promise->future()
        .onDiscard([promise, future]() mutable {
          promise->discard();
          future.discard();
        });
    }))
    .onAny(defer(self(), [this](const Future<ProvisionInfo>&) {
      rwLock.read_unlock();
    }));
}


Future<ProvisionInfo> ProvisionerProcess::_provision(
    const ContainerID& containerId,
    const Image& image,
    const string& backend,
    const ImageInfo& imageInfo)
{
  CHECK(backends.contains(backend));

  string rootfsId = id::UUID::random().toString();

  string rootfs = provisioner::paths::getContainerRootfsDir(
      rootDir,
      containerId,
      backend,
      rootfsId);

  LOG(INFO) << "Provisioning image rootfs '" << rootfs
            << "' for container " << containerId
            << " using " << backend << " backend";

  // NOTE: It's likely that the container ID already exists in 'infos'
  // because one container might provision multiple images.
  if (!infos.contains(containerId)) {
    infos.put(containerId, Owned<Info>(new Info()));
  }

  infos[containerId]->rootfses[backend].insert(rootfsId);
  infos[containerId]->layers = imageInfo.layers;

  if (imageInfo.config.isSome()) {
    infos[containerId]->layers->push_back(imageInfo.config.get());
  }

  string backendDir = provisioner::paths::getBackendDir(
      rootDir,
      containerId,
      backend);

  infos[containerId]->provisioning = backends.at(backend)->provision(
      imageInfo.layers,
      rootfs,
      backendDir)
    .then(defer(self(), [=](const Option<vector<Path>>& ephemeral)
    -> Future<ProvisionInfo> {
      const string path =
        provisioner::paths::getLayersFilePath(rootDir, containerId);

      ContainerLayers containerLayers;

      foreach (const string& layer, imageInfo.layers) {
        containerLayers.add_paths(layer);
      }

      if (imageInfo.config.isSome()) {
        containerLayers.set_config(imageInfo.config.get());
      }

      Try<Nothing> checkpoint = slave::state::checkpoint(path, containerLayers);
      if (checkpoint.isError()) {
        return Failure(
            "Failed to checkpoint layers to '" + path + "': " +
            checkpoint.error());
      }

      return ProvisionInfo{
          rootfs, ephemeral, imageInfo.dockerManifest, imageInfo.appcManifest};
    }));

  return infos[containerId]->provisioning;
}


Future<bool> ProvisionerProcess::destroy(const ContainerID& containerId)
{
  // `destroy` and `provision` can happen concurrently, but `pruneImages`
  // is exclusive.
  return rwLock.read_lock()
    .then(defer(self(), [this, containerId]() -> Future<bool> {
      if (!infos.contains(containerId)) {
        VLOG(1) << "Ignoring destroy request for unknown container "
                << containerId;

        return false;
      }

      if (infos[containerId]->destroying) {
        return infos[containerId]->termination.future();
      }

      infos[containerId]->destroying = true;

      // Provisioner destroy can be invoked from:
      // 1. Provisioner `recover` to destroy all unknown orphans.
      // 2. Containerizer `recover` to destroy known orphans.
      // 3. Containerizer `destroy` on one specific container.
      //
      // NOTE: For (2) and (3), we expect the container being destroyed
      // has no any child contain remain running. However, for case (1),
      // if the container runtime directory does not survive after the
      // machine reboots and the provisioner directory under the agent
      // work dir still exists, all containers will be regarded as
      // unknown containers and will be destroyed. In this case, a parent
      // container may be destroyed before its child containers are
      // cleaned up. So we have to make `destroy()` recursively for
      // this particular case.
      //
      // TODO(gilbert): Move provisioner directory to the container
      // runtime directory after a deprecation cycle to avoid
      // making `provisioner::destroy()` being recursive.
      vector<Future<bool>> destroys;

      foreachkey (const ContainerID& entry, infos) {
        if (entry.has_parent() && entry.parent() == containerId) {
          destroys.push_back(destroy(entry));
        }
      }

      return await(destroys)
        .then(defer(self(), &Self::_destroy, containerId, lambda::_1));
    }))
    .onAny(defer(self(), [this](const Future<bool>&) {
      rwLock.read_unlock();
    }));
}


Future<bool> ProvisionerProcess::_destroy(
    const ContainerID& containerId,
    const vector<Future<bool>>& destroys)
{
  CHECK(infos.contains(containerId));
  CHECK(infos[containerId]->destroying);

  vector<string> errors;
  foreach (const Future<bool>& future, destroys) {
    if (!future.isReady()) {
      errors.push_back(future.isFailed()
        ? future.failure()
        : "discarded");
    }
  }

  if (!errors.empty()) {
    ++metrics.remove_container_errors;

    return Failure(
        "Failed to destroy nested containers: " +
        strings::join("; ", errors));
  }

  const Owned<Info>& info = infos[containerId];

  info->provisioning
    .onAny(defer(self(), [=](const Future<ProvisionInfo>&) -> void {
      vector<Future<bool>> futures;
      foreachkey (const string& backend, info->rootfses) {
        if (!backends.contains(backend)) {
          infos[containerId]->termination.fail(
              "Unknown backend '" + backend + "'");

          return;
        }

        foreach (const string& rootfsId, info->rootfses[backend]) {
          string rootfs = provisioner::paths::getContainerRootfsDir(
              rootDir,
              containerId,
              backend,
              rootfsId);

          string backendDir = provisioner::paths::getBackendDir(
              rootDir,
              containerId,
              backend);

          LOG(INFO) << "Destroying container rootfs at '" << rootfs
                    << "' for container " << containerId;

          futures.push_back(
              backends.at(backend)->destroy(rootfs, backendDir));
        }
      }

      await(futures)
        .onAny(defer(
            self(),
            &ProvisionerProcess::__destroy,
            containerId,
            lambda::_1));
    }));

  return info->termination.future();
}


void ProvisionerProcess::__destroy(
    const ContainerID& containerId,
    const Future<vector<Future<bool>>>& futures)
{
  CHECK(infos.contains(containerId));
  CHECK(infos[containerId]->destroying);

  CHECK_READY(futures);

  vector<string> messages;
  foreach (const Future<bool>& future, futures.get()) {
    if (!future.isReady()) {
      messages.push_back(
          future.isFailed() ? future.failure() : "discarded");
    }
  }

  if (!messages.empty()) {
    infos[containerId]->termination.fail(strings::join("\n", messages));
    return;
  }

  // This should be fairly cheap as the directory should only
  // contain a few empty sub-directories at this point.
  //
  // TODO(jieyu): Currently, it's possible that some directories
  // cannot be removed due to EBUSY. EBUSY is caused by the race
  // between cleaning up this container and new containers copying
  // the host mount table. It's OK to ignore them. The cleanup
  // will be retried during slave recovery.
  string containerDir =
    provisioner::paths::getContainerDir(rootDir, containerId);

  Try<Nothing> rmdir = os::rmdir(containerDir);
  if (rmdir.isError()) {
    LOG(ERROR) << "Failed to remove the provisioned container directory "
               << "at '" << containerDir << "': " << rmdir.error();

    ++metrics.remove_container_errors;
  }

  infos[containerId]->termination.set(true);
  infos.erase(containerId);
}


Future<Nothing> ProvisionerProcess::pruneImages(
    const vector<Image>& excludedImages)
{
  // `destroy` and `provision` can happen concurrently, but `pruneImages`
  // is exclusive.
  return rwLock.write_lock()
    .then(defer(self(), [this, excludedImages]() -> Future<Nothing> {
      hashset<string> activeLayerPaths;

      foreachpair (
          const ContainerID& containerId, const Owned<Info>& info, infos) {
        if (info->layers.isNone()) {
          // There are several possibilities if layer information missing:
          // - legacy containers provisioned before layer checkpointing:
          //   they should already be excluded by the containerizer;
          // - the agent crashed after `backend::provision()` finished but
          //   before checkpointing the `layers`. In such a case, the rootfs
          //   should not be used by any running containers yet so it is safe
          //   to skip those layers;
          // - checkpointed layer files were manually deleted: we do not expect
          //   this to be allowd, but log it for information purpose.
          VLOG(1) << "Container " << containerId
                  << " has no checkpointed layers";

          continue;
        }

        activeLayerPaths.insert(info->layers->begin(), info->layers->end());
      }

      vector<Future<Nothing>> futures;

      foreachpair (
          const Image::Type& type, const Owned<Store>& store, stores) {
        vector<Image> images;
        foreach (const Image& image, excludedImages) {
          if (image.type() == type) {
            images.push_back(image);
          }
        }

        futures.push_back(store->prune(images, activeLayerPaths));
      }

      return collect(futures)
        .then([]() { return Nothing(); });
    }))
    .onAny(defer(self(), [this](const Future<Nothing>&) {
      rwLock.write_unlock();
    }));
}


ProvisionerProcess::Metrics::Metrics()
  : remove_container_errors(
      "containerizer/mesos/provisioner/remove_container_errors")
{
  process::metrics::add(remove_container_errors);
}


ProvisionerProcess::Metrics::~Metrics()
{
  process::metrics::remove(remove_container_errors);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
