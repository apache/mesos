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

#include <process/dispatch.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/os.hpp>

#include "linux/fs.hpp"

#include "slave/containerizer/provisioner/backends/overlay.hpp"

using namespace process;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

class OverlayBackendProcess : public Process<OverlayBackendProcess>
{
public:
  Future<Nothing> provision(const vector<string>& layers, const string& rootfs);

  Future<bool> destroy(const string& rootfs);
};


Try<Owned<Backend>> OverlayBackend::create(const Flags&)
{
  Result<string> user = os::user();
  if (!user.isSome()) {
    return Error("Failed to determine user: " +
                 (user.isError() ? user.error() : "username not found"));
  }

  if (user.get() != "root") {
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
    const string& rootfs)
{
  return dispatch(
      process.get(), &OverlayBackendProcess::provision, layers, rootfs);
}


Future<bool> OverlayBackend::destroy(const string& rootfs)
{
  return dispatch(process.get(), &OverlayBackendProcess::destroy, rootfs);
}


Future<Nothing> OverlayBackendProcess::provision(
    const vector<string>& layers,
    const string& rootfs)
{
  std::ifstream filesystem("/proc/filesystems");
  std::string line;
  std::string str("nodev\toverlay");
  bool overlay_supported = false;
  while (std::getline(filesystem, line))
  {
      if (line.compare(str) == 0) {
        overlay_supported = true;
      }
  }

  if (!overlay_supported) {
    return Failure("Overlay filesystem not supported");
  }

  if (layers.size() == 0) {
    return Failure("No filesystem layer provided");
  }

  if (layers.size() == 1) {
    return Failure("Need more than one image for overlay")
  }

  Try<Nothing> mkdir = os::mkdir(rootfs);
  if (mkdir.isError()) {
    return Failure("Failed to create container rootfs at " + rootfs);
  }

  // The specified lower directories will be stacked beginning from the
  // rightmost one and going left. The first layer in the vector will be
  // the bottom most layer (i.e. applied first).
  std::string lowerDir = "lowerdir=";
  lowerDir += strings::join(":", layers);

  Try<Nothing> mount = fs::mount(
      "overlay",
      mountpoint,
      "overlay",
      MS_RDONLY,
      lowerDir);

  if (mount.isError()) {
    return Failure("Failed to remount rootfs '" + rootfs + "' read-only: " +
        mount.error()););
  }

  return Nothing();
}


Future<bool> OverlayBackendProcess::destroy(const string& rootfs)
{
  Try<fs::MountInfoTable> mountTable = fs::MountInfoTable::read();

  if (mountTable.isError()) {
    return Failure("Failed to read mount table: " + mountTable.error());
  }

  foreach (const fs::MountInfoTable::Entry& entry, mountTable.get().entries) {
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

      return true;
    }
  }

  return false;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
