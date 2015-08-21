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

#ifndef __TEST_ROOTFS_HPP__
#define __TEST_ROOTFS_HPP__

#include <string>
#include <vector>

#include <process/owned.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace tests {

class Rootfs {
public:
  virtual ~Rootfs()
  {
    if (os::exists(root)) {
      os::rmdir(root);
    }
  }

  // Add a host directory or file to the root filesystem. Note that
  // the host directory or file needs to be an absolute path.
  Try<Nothing> add(const std::string& path)
  {
    if (!os::exists(path)) {
      return Error("File or directory not found on the host");
    }

    if (!strings::startsWith(path, "/")) {
      return Error("Not an absolute path");
    }

    // TODO(jieyu): Make sure 'path' is not under 'root'.

    if (os::stat::isdir(path)) {
      if (os::system("cp -r '" + path + "' '" + root + "'") != 0) {
        return ErrnoError("Failed to copy '" + path + "' to rootfs");
      }
    } else if (os::stat::isfile(path)) {
      std::string dirname = Path(path).dirname();
      std::string target = path::join(root, dirname);

      Try<Nothing> mkdir = os::mkdir(target);
      if (mkdir.isError()) {
        return Error("Failed to create directory in rootfs: " + mkdir.error());
      }

      if (os::system("cp '" + path + "' '" + target + "'") != 0) {
        return ErrnoError("Failed to copy '" + path + "' to rootfs");
      }
    } else {
      return Error("Unsupported file or directory");
    }

    return Nothing();
  }

  const std::string root;

protected:
  Rootfs(const std::string& _root) : root(_root) {}
};


class LinuxRootfs : public Rootfs
{
public:
  static Try<process::Owned<Rootfs>> create(const std::string& root)
  {
    process::Owned<Rootfs> rootfs(new LinuxRootfs(root));

    if (!os::exists(root)) {
      Try<Nothing> mkdir = os::mkdir(root);
      if (mkdir.isError()) {
        return Error("Failed to create root directory: " + mkdir.error());
      }
    }

    std::vector<std::string> directories = {
      "/bin",
      "/lib",
      "/lib64"
    };

    foreach (const std::string& directory, directories) {
      // Some linux distros are moving all binaries and libraries to
      // /usr, in which case /bin, /lib, and /lib64 will be symlinks
      // to their equivalent directories in /usr.
      Result<std::string> realpath = os::realpath(directory);
      if (!realpath.isSome()) {
        return Error("Failed to get realpath for '" +
                     directory + "': " + (realpath.isError() ?
                     realpath.error() : "No such directory"));
      }

      Try<Nothing> result = rootfs->add(realpath.get());
      if (result.isError()) {
        return Error("Failed to add '" + realpath.get() +
                     "' to rootfs: " + result.error());
      }
    }

    directories = {
      "/proc",
      "/sys",
      "/dev",
      "/tmp"
    };

    foreach (const std::string& directory, directories) {
      Try<Nothing> mkdir = os::mkdir(path::join(root, directory));
      if (mkdir.isError()) {
        return Error("Failed to create '" + directory +
                     "' in rootfs: " + mkdir.error());
      }
    }

    return rootfs;
  }

protected:
  LinuxRootfs(const std::string& root) : Rootfs(root) {}
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_ROOTFS_HPP__
