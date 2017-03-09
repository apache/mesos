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

#include <list>

#include <mesos/docker/spec.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include <stout/foreach.hpp>
#include <stout/os.hpp>

#include <stout/os/constants.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/mesos/provisioner/backends/copy.hpp"

using namespace process;

using std::string;
using std::list;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

class CopyBackendProcess : public Process<CopyBackendProcess>
{
public:
  CopyBackendProcess()
    : ProcessBase(process::ID::generate("copy-provisioner-backend")) {}

  Future<Nothing> provision(const vector<string>& layers, const string& rootfs);

  Future<bool> destroy(const string& rootfs);

private:
  Future<Nothing> _provision(string layer, const string& rootfs);
};


Try<Owned<Backend>> CopyBackend::create(const Flags&)
{
  return Owned<Backend>(new CopyBackend(
      Owned<CopyBackendProcess>(new CopyBackendProcess())));
}


CopyBackend::~CopyBackend()
{
  terminate(process.get());
  wait(process.get());
}


CopyBackend::CopyBackend(Owned<CopyBackendProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Future<Nothing> CopyBackend::provision(
    const vector<string>& layers,
    const string& rootfs,
    const string& backendDir)
{
  return dispatch(
      process.get(), &CopyBackendProcess::provision, layers, rootfs);
}


Future<bool> CopyBackend::destroy(
    const string& rootfs,
    const string& backendDir)
{
  return dispatch(process.get(), &CopyBackendProcess::destroy, rootfs);
}


Future<Nothing> CopyBackendProcess::provision(
    const vector<string>& layers,
    const string& rootfs)
{
  if (layers.size() == 0) {
    return Failure("No filesystem layers provided");
  }

  if (os::exists(rootfs)) {
    return Failure("Rootfs is already provisioned");
  }

  Try<Nothing> mkdir = os::mkdir(rootfs);
  if (mkdir.isError()) {
    return Failure("Failed to create rootfs directory: " + mkdir.error());
  }

  list<Future<Nothing>> futures{Nothing()};

  foreach (const string layer, layers) {
    futures.push_back(
        futures.back().then(
            defer(self(), &Self::_provision, layer, rootfs)));
  }

  return collect(futures)
    .then([]() -> Future<Nothing> { return Nothing(); });
}


Future<Nothing> CopyBackendProcess::_provision(
    string layer,
    const string& rootfs)
{
#ifndef __WINDOWS__
  // Traverse the layer to check if there is any whiteout files, if
  // yes, remove the corresponding files/directories from the rootfs.
  // Note: We assume all image types use AUFS whiteout format.
  char* source[] = {const_cast<char*>(layer.c_str()), nullptr};

  FTS* tree = ::fts_open(source, FTS_NOCHDIR | FTS_PHYSICAL, nullptr);
  if (tree == nullptr) {
    return Failure("Failed to open '" + layer + "': " + os::strerror(errno));
  }

  vector<string> whiteouts;
  for (FTSENT *node = ::fts_read(tree);
       node != nullptr; node = ::fts_read(tree)) {
    if (node->fts_info != FTS_F) {
      continue;
    }

    if (!strings::startsWith(node->fts_name, docker::spec::WHITEOUT_PREFIX)) {
      continue;
    }

    string ftsPath = string(node->fts_path);
    Path whiteout = Path(ftsPath.substr(layer.length() + 1));

    // Keep the relative paths of the whiteout files, we will
    // remove them from rootfs after layer is copied to rootfs.
    whiteouts.push_back(whiteout.string());

    if (node->fts_name == string(docker::spec::WHITEOUT_OPAQUE_PREFIX)) {
      const string path = path::join(rootfs, Path(whiteout).dirname());

      // Remove the entries under the directory labeled
      // as opaque whiteout from rootfs.
      Try<Nothing> rmdir = os::rmdir(path, true, false);
      if (rmdir.isError()) {
        ::fts_close(tree);
        return Failure(
            "Failed to remove the entries under the directory labeled as"
            " opaque whiteout '" + path + "': " + rmdir.error());
      }
    } else {
      const string path = path::join(
          rootfs,
          whiteout.dirname(),
          whiteout.basename().substr(strlen(docker::spec::WHITEOUT_PREFIX)));

      // The file/directory labeled as whiteout may have already been
      // removed with the code above due to its parent directory labeled
      // as opaque whiteout, so here we need to check if it still exists
      // before trying to remove it.
      if (os::exists(path)) {
        if (os::stat::isdir(path)) {
          Try<Nothing> rmdir = os::rmdir(path);
          if (rmdir.isError()) {
            ::fts_close(tree);
            return Failure(
                "Failed to remove the directory labeled as whiteout '" +
                path + "': " + rmdir.error());
          }
        } else {
          Try<Nothing> rm = os::rm(path);
          if (rm.isError()) {
            ::fts_close(tree);
            return Failure(
                "Failed to remove the file labeled as whiteout '" +
                path + "': " + rm.error());
          }
        }
      }
    }
  }

  if (errno != 0) {
    Error error = ErrnoError();
    ::fts_close(tree);
    return Failure(error);
  }

  if (::fts_close(tree) != 0) {
    return Failure(
        "Failed to stop traversing file system: " + os::strerror(errno));
  }

  VLOG(1) << "Copying layer path '" << layer << "' to rootfs '" << rootfs
          << "'";

#if defined(__APPLE__) || defined(__FreeBSD__)
  if (!strings::endsWith(layer, "/")) {
    layer += "/";
  }

  // BSD cp doesn't support -T flag, but supports source trailing
  // slash so we only copy the content but not the folder.
  vector<string> args{"cp", "-a", layer, rootfs};
#else
  vector<string> args{"cp", "-aT", layer, rootfs};
#endif // __APPLE__ || __FreeBSD__

  Try<Subprocess> s = subprocess(
      "cp",
      args,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to create 'cp' subprocess: " + s.error());
  }

  Subprocess cp = s.get();

  return cp.status()
    .then([=](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure("Failed to reap subprocess to copy image");
      } else if (status.get() != 0) {
        return io::read(cp.err().get())
          .then([](const string& err) -> Future<Nothing> {
            return Failure("Failed to copy layer: " + err);
          });
      }

      // Remove the whiteout files from rootfs.
      foreach (const string whiteout, whiteouts) {
        Try<Nothing> rm = os::rm(path::join(rootfs, whiteout));
        if (rm.isError()) {
          return Failure(
              "Failed to remove whiteout file '" +
              whiteout + "': " + rm.error());
        }
      }

      return Nothing();
    });
#else
  return Failure(
      "Provisioning a rootfs from an image is not supported on Windows");
#endif // __WINDOWS__
}


Future<bool> CopyBackendProcess::destroy(const string& rootfs)
{
  vector<string> argv{"rm", "-rf", rootfs};

  Try<Subprocess> s = subprocess(
      "rm",
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO));

  if (s.isError()) {
    return Failure("Failed to create 'rm' subprocess: " + s.error());
  }

  return s.get().status()
    .then([](const Option<int>& status) -> Future<bool> {
      if (status.isNone()) {
        return Failure("Failed to reap subprocess to destroy rootfs");
      } else if (status.get() != 0) {
        return Failure("Failed to destroy rootfs, exit status: " +
                       WSTRINGIFY(status.get()));
      }

      return true;
    });
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
