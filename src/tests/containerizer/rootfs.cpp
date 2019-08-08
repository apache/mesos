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

#include "rootfs.hpp"

#include <vector>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>

#include <stout/os/realpath.hpp>
#include <stout/os/stat.hpp>

#include "linux/ldd.hpp"

using std::string;
using std::vector;

using process::Owned;

namespace mesos {
namespace internal {
namespace tests {

Rootfs::~Rootfs()
{
  if (os::exists(root)) {
    os::rmdir(root);
  }
}


Try<Nothing> Rootfs::add(const string& path)
{
  Option<string> source = None();

  // If we are copying a symlink, follow it and copy the
  // target instead. While this is a little inefficient on
  // disk space, it avoids complexity in dealing with chains
  // of symlinks and symlinks in intermediate path components.
  if (os::stat::islink(path)) {
    Result<string> target = os::realpath(path);
    if (target.isNone()) {
      return Error("Failed to resolve '" + path + "'");
    }

    if (target.isError()) {
      return Error("Failed to resolve '" + path + "': " + target.error());
    }

    source = target.get();
  }

  Try<Nothing> copy = copyPath(source.isSome() ? source.get() : path, path);
  if (copy.isError()) {
    return Error("Failed to copy '" + path + "' to rootfs: " + copy.error());
  }

  return Nothing();
}


Try<Nothing> Rootfs::copyPath(const string& source, const string& destination)
{
  if (!os::exists(source)) {
    return Error("'" + source + "' not found");
  }

  if (!strings::startsWith(source, "/")) {
    return Error("'" + source + "' is not an absolute path");
  }

  string rootfsDestination = path::join(root, destination);
  string rootfsDirectory = Path(rootfsDestination).dirname();

  if (!os::exists(rootfsDirectory)) {
    Try<Nothing> mkdir = os::mkdir(rootfsDirectory);
    if (mkdir.isError()) {
      return Error(
          "Failed to create directory '" + rootfsDirectory +
          "': " + mkdir.error());
    }
  }

  // Copy the files. We preserve all attributes so that e.g., `ping`
  // keeps its file-based capabilities.
  if (os::spawn(
          "cp",
          {"cp", "-r", "--preserve=all", source, rootfsDestination}) != 0) {
    return Error("Failed to copy '" + source + "' to rootfs");
  }

  return Nothing();
}


Try<process::Owned<Rootfs>> LinuxRootfs::create(const string& root)
{
  process::Owned<Rootfs> rootfs(new LinuxRootfs(root));

  if (!os::exists(root)) {
    Try<Nothing> mkdir = os::mkdir(root);
    if (mkdir.isError()) {
      return Error("Failed to create root directory: " + mkdir.error());
    }
  }

  Try<vector<ldcache::Entry>> cache = ldcache::parse();

  if (cache.isError()) {
    return Error("Failed to parse ld.so cache: " + cache.error());
  }

  const vector<string> programs = {
    "/bin/cat",
    "/bin/dd",
    "/bin/echo",
    "/bin/ls",
    "/bin/mkdir",
    "/bin/ping",
    "/bin/rm",
    "/bin/sh",
    "/bin/sleep",
  };

  hashset<string> files = {
    "/etc/passwd"
  };

  foreach (const string& program, programs) {
    Try<hashset<string>> dependencies = ldd(program, cache.get());
    if (dependencies.isError()) {
      return Error(
          "Failed to find dependencies for '" + program + "': " +
          dependencies.error());
    }

    files |= dependencies.get();
    files.insert(program);
  }

  foreach (const string& file, files) {
    Try<Nothing> result = rootfs->add(file);
    if (result.isError()) {
      return Error(result.error());
    }
  }

  const vector<string> directories = {
    "/proc",
    "/sys",
    "/dev",
    "/tmp"
  };

  foreach (const string& directory, directories) {
    Try<Nothing> mkdir = os::mkdir(path::join(root, directory));
    if (mkdir.isError()) {
      return Error(
          "Failed to create '" + directory + "' in rootfs: " +
          mkdir.error());
    }
  }

  return rootfs;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
