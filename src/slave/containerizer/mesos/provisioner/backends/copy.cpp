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

#include <vector>

#ifndef __WINDOWS__
#include <mesos/docker/spec.hpp>
#endif // __WINDOWS__

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
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

class CopyBackendProcess : public Process<CopyBackendProcess>
{
public:
  CopyBackendProcess()
    : ProcessBase(process::ID::generate("copy-provisioner-backend")) {}

  Future<Option<vector<Path>>> provision(
      const vector<string>& layers, const string& rootfs);

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


Future<Option<vector<Path>>> CopyBackend::provision(
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


Future<Option<vector<Path>>> CopyBackendProcess::provision(
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

  vector<Future<Nothing>> futures{Nothing()};

  foreach (const string& layer, layers) {
    futures.push_back(
        futures.back().then(
            defer(self(), &Self::_provision, layer, rootfs)));
  }

  return collect(futures)
    .then([]() -> Future<Option<vector<Path>>> { return None(); });
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
    string ftsPath = string(node->fts_path);

    if (node->fts_info == FTS_DNR ||
        node->fts_info == FTS_ERR ||
        node->fts_info == FTS_NS) {
      ::fts_close(tree);
      return Failure(
          "Failed to read '" + ftsPath + "': " + os::strerror(node->fts_errno));
    }

    // Skip the postorder visit of a directory.
    // See the manpage of fts_read in the following link:
    //   http://man7.org/linux/man-pages/man3/fts_read.3.html
    if (node->fts_info == FTS_DP) {
      continue;
    }

    if (ftsPath == layer) {
      continue;
    }

    string layerPath = ftsPath.substr(layer.length() + 1);
    string rootfsPath = path::join(rootfs, layerPath);
    Option<string> removePath;

    // Handle whiteout files.
    if (node->fts_info == FTS_F &&
        strings::startsWith(node->fts_name, docker::spec::WHITEOUT_PREFIX)) {
      Path whiteout = Path(layerPath);

      // Keep the absolute paths of the whiteout files, we will
      // remove them from rootfs after layer is copied to rootfs.
      whiteouts.push_back(rootfsPath);

      if (node->fts_name == string(docker::spec::WHITEOUT_OPAQUE_PREFIX)) {
        removePath = path::join(rootfs, whiteout.dirname());
      } else {
        removePath = path::join(
            rootfs,
            whiteout.dirname(),
            whiteout.basename().substr(strlen(docker::spec::WHITEOUT_PREFIX)));
      }
    }

    if (os::exists(rootfsPath)) {
      bool ftsIsDir = node->fts_info == FTS_D || node->fts_info == FTS_DC;
      if (os::stat::isdir(rootfsPath) != ftsIsDir) {
        // Handle overwriting between a directory and a non-directory.
        // Note: If a symlink is overwritten by a directory, the symlink
        // must be removed before the directory is traversed so the
        // following case won't cause a security issue:
        //   ROOTFS: /bad@ -> /usr
        //   LAYER:  /bad/bin/.wh.wh.opq
        removePath = rootfsPath;
      } else if (os::stat::islink(rootfsPath)) {
        // Handle overwriting a symlink with a regular file.
        // Note: The symlink must be removed, or 'cp' would follow the
        // link and overwrite the target instead of the link itself,
        // which would cause a security issue in the following case:
        //   ROOTFS: /bad@ -> /usr/bin/python
        //   LAYER:  /bad is a malicious executable
        removePath = rootfsPath;
      }
    }

    // The file/directory referred to by removePath may be empty or have
    // already been removed because its parent directory is labeled as
    // opaque whiteout or overwritten by a file, so here we need to
    // check if it exists before trying to remove it.
    if (removePath.isSome() && os::exists(removePath.get())) {
      if (os::stat::isdir(removePath.get())) {
        // It is OK to remove the entire directory labeled as opaque
        // whiteout, since the same directory exists in this layer and
        // will be copied back to rootfs.
        Try<Nothing> rmdir = os::rmdir(removePath.get());
        if (rmdir.isError()) {
          ::fts_close(tree);
          return Failure(
              "Failed to remove directory '" +
              removePath.get() + "': " + rmdir.error());
        }
      } else {
        Try<Nothing> rm = os::rm(removePath.get());
        if (rm.isError()) {
          ::fts_close(tree);
          return Failure(
              "Failed to remove file '" +
              removePath.get() + "': " + rm.error());
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
      foreach (const string& whiteout, whiteouts) {
        Try<Nothing> rm = os::rm(whiteout);
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

  return s->status()
    .then([](const Option<int>& status) -> Future<bool> {
      if (status.isNone()) {
        return Failure("Failed to reap subprocess to destroy rootfs");
      }

      if (status.get() != 0) {
        // It's possible that `rm -rf` will fail if some other
        // programs are accessing the files.  No need to return a hard
        // failure here because the directory will be removed later
        // and re-attempted on agent recovery.
        LOG(ERROR) << "Failed to destroy rootfs, exit status: "
                   << WSTRINGIFY(status.get());
      }

      return true;
    });
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
