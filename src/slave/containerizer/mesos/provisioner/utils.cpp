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


#include <fts.h>

#include <sys/stat.h>

#include <string>
#include <string.h>

#include <mesos/docker/spec.hpp>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/rm.hpp>
#include <stout/os/strerror.hpp>
#include <stout/os/xattr.hpp>

#include "slave/containerizer/mesos/provisioner/utils.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace slave {

#ifdef __linux__
// This function is used to convert AUFS whiteouts to OverlayFS whiteouts and
// the AUFS whiteouts will be removed after the conversion is done. Whiteout is
// for hiding directories and files in the lower level layers. AUFS whiteout is
// the whiteout standard in Docker, and it relies on '.wh..wh..opq' to hide
// directory and '.wh.<filename>' to hide file, whereas OverlayFS relies on
// setting an extended attribute 'trusted.overlay.opaque' on the directory to
// hide it and creating character device with 0/0 device number to hide file.
// See the links below for more details:
// http://aufs.sourceforge.net/aufs.html
// https://www.kernel.org/doc/Documentation/filesystems/overlayfs.txt
Try<Nothing> convertWhiteouts(const string& directory)
{
  char* rootfs[] = {const_cast<char*>(directory.c_str()), nullptr};

  FTS* tree = ::fts_open(rootfs, FTS_NOCHDIR | FTS_PHYSICAL, nullptr);
  if (tree == nullptr) {
    return Error("Failed to open '" + directory + "': " + os::strerror(errno));
  }

  for (FTSENT *node = ::fts_read(tree);
       node != nullptr; node = ::fts_read(tree)) {
    if (node->fts_info != FTS_F) {
      continue;
    }

    if (!strings::startsWith(node->fts_name, docker::spec::WHITEOUT_PREFIX)) {
      continue;
    }

    const Path path = Path(node->fts_path);
    if (node->fts_name == string(docker::spec::WHITEOUT_OPAQUE_PREFIX)) {
      Try<Nothing> setxattr = os::setxattr(
          path.dirname(),
          "trusted.overlay.opaque",
          "y",
          0);

      if (setxattr.isError()) {
        ::fts_close(tree);
        return Error(
            "Failed to set extended attribute 'trusted.overlay.opaque'"
            " for the directory '" + path.dirname() + "': " +
            setxattr.error());
      }
    } else {
      const string originalPath = path::join(
          path.dirname(),
          path.basename().substr(strlen(docker::spec::WHITEOUT_PREFIX)));

      Try<Nothing> mknod = os::mknod(originalPath, S_IFCHR, 0);
      if (mknod.isError()) {
        ::fts_close(tree);
        return Error(
            "Failed to create character device '" +
            originalPath + "': " + mknod.error());
      }
    }

    // Remove the AUFS whiteout file.
    Try<Nothing> rm = os::rm(path.string());
    if (rm.isError()) {
      ::fts_close(tree);
      return Error(
          "Failed to remove AUFS whiteout file '" +
          path.string() + "': " + rm.error());
    }
  }

  if (errno != 0) {
    Error error = ErrnoError();
    ::fts_close(tree);
    return error;
  }

  if (::fts_close(tree) != 0) {
    return Error(
        "Failed to stop traversing file system: " + os::strerror(errno));
  }

  return Nothing();
}
#endif

} // namespace slave {
} // namespace internal {
} // namespace mesos {
