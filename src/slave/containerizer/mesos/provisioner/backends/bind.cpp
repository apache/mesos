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

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/foreach.hpp>
#include <stout/os.hpp>

#include "linux/fs.hpp"

#include "slave/containerizer/mesos/provisioner/backends/bind.hpp"

using namespace process;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

class BindBackendProcess : public Process<BindBackendProcess>
{
public:
  BindBackendProcess()
    : ProcessBase(process::ID::generate("bind-provisioner-backend")) {}

  Future<Option<vector<Path>>> provision(
      const vector<string>& layers, const string& rootfs);

  Future<bool> destroy(const string& rootfs);

  struct Metrics
  {
    Metrics();
    ~Metrics();

    process::metrics::Counter remove_rootfs_errors;
  } metrics;
};


Try<Owned<Backend>> BindBackend::create(const Flags&)
{
  if (geteuid() != 0) {
    return Error("BindBackend requires root privileges");
  }

  return Owned<Backend>(new BindBackend(
      Owned<BindBackendProcess>(new BindBackendProcess())));
}


BindBackend::~BindBackend()
{
  terminate(process.get());
  wait(process.get());
}


BindBackend::BindBackend(Owned<BindBackendProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Future<Option<std::vector<Path>>> BindBackend::provision(
    const vector<string>& layers,
    const string& rootfs,
    const string& backendDir)
{
  return dispatch(
      process.get(), &BindBackendProcess::provision, layers, rootfs);
}


Future<bool> BindBackend::destroy(
    const string& rootfs,
    const string& backendDir)
{
  return dispatch(process.get(), &BindBackendProcess::destroy, rootfs);
}


Future<Option<std::vector<Path>>> BindBackendProcess::provision(
    const vector<string>& layers,
    const string& rootfs)
{
  if (layers.size() > 1) {
    return Failure(
        "Multiple layers are not supported by the bind backend");
  }

  if (layers.size() == 0) {
    return Failure("No filesystem layer provided");
  }

  Try<Nothing> mkdir = os::mkdir(rootfs);
  if (mkdir.isError()) {
    return Failure("Failed to create container rootfs at " + rootfs);
  }

  // TODO(xujyan): Use MS_REC? Does any provisioner use mounts within
  // its image store in a single layer?
  Try<Nothing> mount = fs::mount(
      layers.front(),
      rootfs,
      None(),
      MS_BIND | MS_RDONLY,
      nullptr);

  if (mount.isError()) {
    return Failure(
        "Failed to bind mount rootfs '" + layers.front() +
        "' to '" + rootfs + "': " + mount.error());
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

  return None();
}


Future<bool> BindBackendProcess::destroy(const string& rootfs)
{
  Try<fs::MountInfoTable> mountTable = fs::MountInfoTable::read();

  if (mountTable.isError()) {
    return Failure("Failed to read mount table: " + mountTable.error());
  }

  foreach (const fs::MountInfoTable::Entry& entry, mountTable->entries) {
    // TODO(xujyan): If MS_REC was used in 'provision()' we would need
    // to check `strings::startsWith(entry.target, rootfs)` here to
    // unmount all nested mounts.
    if (entry.target == rootfs) {
      // NOTE: Use MNT_DETACH here so that if there are still
      // processes holding files or directories in the rootfs, the
      // unmount will still be successful. The kernel will cleanup the
      // mount when the number of references reach zero.
      Try<Nothing> unmount = fs::unmount(entry.target, MNT_DETACH);
      if (unmount.isError()) {
        return Failure(
            "Failed to destroy bind-mounted rootfs '" + rootfs + "': " +
            unmount.error());
      }

      // TODO(jieyu): If 'rmdir' here returns EBUSY, we still returns
      // a success. This is currently possible because the parent
      // mount of 'rootfs' might not be a shared mount. Thus,
      // containers in different mount namespaces might hold extra
      // references to this mount. It is OK to ignore the EBUSY error
      // because the provisioner will later try to delete all the
      // rootfses for the terminated containers.
      if (::rmdir(rootfs.c_str()) != 0) {
        string message =
          "Failed to remove rootfs mount point '" + rootfs + "':" +
          os::strerror(errno);

        if (errno == EBUSY) {
          LOG(ERROR) << message;
          ++metrics.remove_rootfs_errors;
        } else {
          return Failure(message);
        }
      }

      return true;
    }
  }

  return false;
}


BindBackendProcess::Metrics::Metrics()
  : remove_rootfs_errors(
      "containerizer/mesos/provisioner/bind/remove_rootfs_errors")
{
  process::metrics::add(remove_rootfs_errors);
}


BindBackendProcess::Metrics::~Metrics()
{
  process::metrics::remove(remove_rootfs_errors);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
