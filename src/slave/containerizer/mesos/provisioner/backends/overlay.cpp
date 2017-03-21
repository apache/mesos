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

#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/adaptor.hpp>
#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/os.hpp>

#include "linux/fs.hpp"

#include "slave/containerizer/mesos/provisioner/backends/overlay.hpp"

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::Shared;

using process::dispatch;
using process::spawn;
using process::wait;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

class OverlayBackendProcess : public Process<OverlayBackendProcess>
{
public:
  OverlayBackendProcess()
    : ProcessBase(process::ID::generate("overlay-provisioner-backend")) {}

  Future<Nothing> provision(
      const vector<string>& layers,
      const string& rootfs,
      const string& backendDir);

  Future<bool> destroy(
      const string& rootfs,
      const string& backendDir);
};


Try<Owned<Backend>> OverlayBackend::create(const Flags&)
{
  if (geteuid() != 0) {
    return Error("OverlayBackend requires root privileges");
  }

  return Owned<Backend>(new OverlayBackend(
      Owned<OverlayBackendProcess>(new OverlayBackendProcess())));
}


OverlayBackend::~OverlayBackend()
{
  terminate(process.get());
  wait(process.get());
}


OverlayBackend::OverlayBackend(Owned<OverlayBackendProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Future<Nothing> OverlayBackend::provision(
    const vector<string>& layers,
    const string& rootfs,
    const string& backendDir)
{
  return dispatch(
      process.get(),
      &OverlayBackendProcess::provision,
      layers,
      rootfs,
      backendDir);
}

Future<bool> OverlayBackend::destroy(
    const string& rootfs,
    const string& backendDir)
{
  return dispatch(
      process.get(),
      &OverlayBackendProcess::destroy,
      rootfs,
      backendDir);
}


Future<Nothing> OverlayBackendProcess::provision(
    const vector<string>& layers,
    const string& rootfs,
    const string& backendDir)
{
  if (layers.size() == 0) {
    return Failure("No filesystem layer provided");
  }

  Try<Nothing> mkdir = os::mkdir(rootfs);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create container rootfs at '" +
        rootfs + "': " + mkdir.error());
  }

  const string rootfsId = Path(rootfs).basename();
  const string scratchDir = path::join(backendDir, "scratch", rootfsId);
  const string upperdir = path::join(scratchDir,  "upperdir");
  const string workdir = path::join(scratchDir, "workdir");

  mkdir = os::mkdir(upperdir);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create overlay upperdir at '" +
        upperdir + "': " + mkdir.error());
  }

  mkdir = os::mkdir(workdir);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create overlay workdir at '" +
        workdir + "': " + mkdir.error());
  }

  // We create symlink with shorter path to each of the base layers.
  Try<string> mktemp = os::mkdtemp();
  if (mktemp.isError()) {
    return Failure(
      "Failued to create temporary directory for symlinks to layers: " +
      mktemp.error());
  }

  const string tempDir = mktemp.get();
  const string tempLink = path::join(scratchDir, "links");

  Try<Nothing> symlink = ::fs::symlink(tempDir, tempLink);
  if (symlink.isError()) {
    return Failure(
        "Failed to create symlink '" + tempLink +
        "' -> '" + tempDir + "': " + symlink.error());
  }

  VLOG(1) << "Created symlink '" << tempLink << "' -> '" << tempDir << "'";

  vector<string> links;
  links.reserve(layers.size());

  // We create symlinks with file name 0, 1, ..., N-1 in tempDir which
  // points to the corresponding layers in the same order.
  size_t idx = 0;
  foreach (const string& layer, layers) {
    const string link = path::join(tempDir, std::to_string(idx++));

    Try<Nothing> symlink = ::fs::symlink(layer, link);
    if (symlink.isError()) {
      return Failure(
          "Failed to create symlink at '" + link +
          "' -> '" + layer + "': " + symlink.error());
    }

    links.push_back(link);
  }

  // For overlayfs, the specified lower directories will be stacked
  // beginning from the rightmost one and going left. But we need the
  // first layer in the vector to be the bottom most layer.
  string options = "lowerdir=" + strings::join(":", adaptor::reverse(links));
  options += ",upperdir=" + upperdir;
  options += ",workdir=" + workdir;

  VLOG(1) << "Provisioning image rootfs with overlayfs: '" << options << "'";

  Try<Nothing> mount = fs::mount(
      "overlay",
      rootfs,
      "overlay",
      0,
      options);

  if (mount.isError()) {
    return Failure(
        "Failed to mount rootfs '" + rootfs +
        "' with overlayfs: " + mount.error());
  }

  // Mark the mount as shared+slave.
  mount = fs::mount(
      None(),
      rootfs,
      None(),
      MS_SLAVE,
      nullptr);

  if (mount.isError()) {
    return Failure(
        "Failed to mark mount '" + rootfs +
        "' as a slave mount: " + mount.error());
  }

  mount = fs::mount(
      None(),
      rootfs,
      None(),
      MS_SHARED,
      nullptr);

  if (mount.isError()) {
    return Failure(
        "Failed to mark mount '" + rootfs +
        "' as a shared mount: " + mount.error());
  }

  return Nothing();
}


Future<bool> OverlayBackendProcess::destroy(
    const string& rootfs,
    const string& backendDir)
{
  Try<fs::MountInfoTable> mountTable = fs::MountInfoTable::read();
  if (mountTable.isError()) {
    return Failure("Failed to read mount table: " + mountTable.error());
  }

  foreach (const fs::MountInfoTable::Entry& entry, mountTable->entries) {
    if (entry.target == rootfs) {
      // NOTE: This would fail if the rootfs is still in use.
      Try<Nothing> unmount = fs::unmount(entry.target);
      if (unmount.isError()) {
        return Failure(
            "Failed to destroy overlay-mounted rootfs '" + rootfs + "': " +
            unmount.error());
      }

      Try<Nothing> rmdir = os::rmdir(rootfs);
      if (rmdir.isError()) {
        return Failure(
            "Failed to remove rootfs mount point '" + rootfs + "': " +
            rmdir.error());
      }

      // Clean up tempDir used for image layer links.
      const string tempLink = path::join(
          backendDir, "scratch", Path(rootfs).basename(), "links");

      if (!os::exists(tempLink)) {
        // TODO(zhitao): This should be converted into a failure after
        // deprecation cycle started by 1.1.0.
        VLOG(1) << "Cannot find symlink to temporary directory '" << tempLink
                <<"' for image links";

        return true;
      }

      if (!os::stat::islink(tempLink)) {
        return Failure("Invalid symlink '" + tempLink + "'");
      }

      Result<string> realpath = os::realpath(tempLink);

      // NOTE: It's possible that the symlink is a dangling symlink.
      // This is possible if agent crashes after we remove the temp
      // directory but before we remove the symlink itself.
      if (realpath.isSome()) {
        Try<Nothing> rmdir = os::rmdir(realpath.get());
        if (rmdir.isError()) {
          return Failure("");
        }

        VLOG(1) << "Removed temporary directory '" << realpath.get()
                << "' pointed by '" << tempLink << "'";
      }

      Try<Nothing> rm = os::rm(tempLink);
      if (rm.isError()) {
        return Failure("Failed to remove symlink at '" + tempLink +
                       "': " + rm.error());
      }

      return true;
    }
  }

  return false;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
