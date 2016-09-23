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
#include <stout/os.hpp>

#include "linux/fs.hpp"

#include "slave/containerizer/mesos/provisioner/backends/aufs.hpp"

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

class AufsBackendProcess : public Process<AufsBackendProcess>
{
public:
  AufsBackendProcess()
    : ProcessBase(process::ID::generate("aufs-provisioner-backend")) {}

  Future<Nothing> provision(
      const vector<string>& layers,
      const string& rootfs,
      const string& backendDir);

  Future<bool> destroy(const string& rootfs);
};


Try<Owned<Backend>> AufsBackend::create(const Flags&)
{
  Result<string> user = os::user();
  if (!user.isSome()) {
    return Error(
        "Failed to determine user: " +
        (user.isError() ? user.error() : "username not found"));
  }

  if (user.get() != "root") {
    return Error(
      "AufsBackend requires root privileges, "
      "but is running as user " + user.get());
  }

  return Owned<Backend>(new AufsBackend(
      Owned<AufsBackendProcess>(new AufsBackendProcess())));
}


AufsBackend::~AufsBackend()
{
  terminate(process.get());
  wait(process.get());
}


AufsBackend::AufsBackend(Owned<AufsBackendProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Future<Nothing> AufsBackend::provision(
    const vector<string>& layers,
    const string& rootfs,
    const string& backendDir)
{
  return dispatch(
      process.get(),
      &AufsBackendProcess::provision,
      layers,
      rootfs,
      backendDir);
}


Future<bool> AufsBackend::destroy(
    const string& rootfs,
    const string& backendDir)
{
  return dispatch(process.get(), &AufsBackendProcess::destroy, rootfs);
}


Future<Nothing> AufsBackendProcess::provision(
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

  const string scratchDirId = Path(rootfs).basename();
  const string scratchDir = path::join(backendDir, "scratch", scratchDirId);

  // The top writable directory for aufs.
  const string workdir = path::join(scratchDir, "workdir");

  mkdir = os::mkdir(workdir);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create aufs workdir at '" +
        workdir + "': " + mkdir.error());
  }

  // See http://aufs.sourceforge.net/aufs2/man.html
  // for the mount syntax for aufs.
  string options = "dirs=" + workdir + ":";

  // For aufs, the specified lower directories will be stacked
  // beginning from the rightmost one and going left. But we need the
  // first layer in the vector to be the bottom most layer.
  options += strings::join(":", adaptor::reverse(layers));

  VLOG(1) << "Provisioning image rootfs with aufs: '" << options << "'";

  Try<Nothing> mount = fs::mount(
      "aufs",
      rootfs,
      "aufs",
      0,
      options);

  if (mount.isError()) {
    return Failure(
        "Failed to mount rootfs '" + rootfs +
        "' with aufs: " + mount.error());
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


Future<bool> AufsBackendProcess::destroy(const string& rootfs)
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
            "Failed to destroy aufs-mounted rootfs '" + rootfs + "': " +
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
