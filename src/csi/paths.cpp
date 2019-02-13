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

#include "csi/paths.hpp"

#include <mesos/type_utils.hpp>

#include <process/address.hpp>
#include <process/http.hpp>

#include <stout/check.hpp>
#include <stout/fs.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <stout/os/realpath.hpp>

namespace http = process::http;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace csi {
namespace paths {

// File names.
constexpr char CONTAINER_INFO_FILE[] = "container.info";
constexpr char ENDPOINT_SOCKET_FILE[] = "endpoint.sock";
constexpr char VOLUME_STATE_FILE[] = "volume.state";


constexpr char CONTAINERS_DIR[] = "containers";
constexpr char VOLUMES_DIR[] = "volumes";
constexpr char MOUNTS_DIR[] = "mounts";
constexpr char STAGING_DIR[] = "staging";
constexpr char TARGET_DIR[] = "target";


constexpr char ENDPOINT_DIR_SYMLINK[] = "endpoint";
constexpr char ENDPOINT_DIR[] = "mesos-csi-XXXXXX";


Try<list<string>> getContainerPaths(
    const string& rootDir,
    const string& type,
    const string& name)
{
  return fs::list(path::join(rootDir, type, name, CONTAINERS_DIR, "*"));
}


string getContainerPath(
    const string& rootDir,
    const string& type,
    const string& name,
    const ContainerID& containerId)
{
  return path::join(
      rootDir,
      type,
      name,
      CONTAINERS_DIR,
      stringify(containerId));
}


Try<ContainerPath> parseContainerPath(const string& rootDir, const string& dir)
{
  // TODO(chhsiao): Consider using `<regex>`, which requires GCC 4.9+.

  // Make sure there's a separator at the end of the `rootDir` so that
  // we don't accidentally slice off part of a directory.
  const string prefix = path::join(rootDir, "");

  if (!strings::startsWith(dir, prefix)) {
    return Error(
        "Directory '" + dir + "' does not fall under the root directory '" +
        rootDir + "'");
  }

  vector<string> tokens = strings::tokenize(
      dir.substr(prefix.size()),
      stringify(os::PATH_SEPARATOR));

  // A complete container path consists of 4 tokens:
  //   <type>/<name>/containers/<volume_id>
  if (tokens.size() != 4 || tokens[2] != CONTAINERS_DIR) {
    return Error(
        "Path '" + path::join(tokens) + "' does not match the structure of a "
        "container path");
  }

  ContainerPath path;
  path.type = tokens[0];
  path.name = tokens[1];
  path.containerId.set_value(tokens[3]);

  return path;
}


string getContainerInfoPath(
    const string& rootDir,
    const string& type,
    const string& name,
    const ContainerID& containerId)
{
  return path::join(
      getContainerPath(rootDir, type, name, containerId),
      CONTAINER_INFO_FILE);
}


string getEndpointDirSymlinkPath(
    const string& rootDir,
    const string& type,
    const string& name,
    const ContainerID& containerId)
{
  return path::join(
      getContainerPath(rootDir, type, name, containerId),
      ENDPOINT_DIR_SYMLINK);
}


Try<string> getEndpointSocketPath(
    const string& rootDir,
    const string& type,
    const string& name,
    const ContainerID& containerId)
{
#ifdef __WINDOWS__
  return Error("CSI is not supported on Windows");
#else
  const string symlinkPath =
    getEndpointDirSymlinkPath(rootDir, type, name, containerId);

  Try<Nothing> mkdir = os::mkdir(Path(symlinkPath).dirname());
  if(mkdir.isError()) {
    return Error(
        "Failed to create directory '" + Path(symlinkPath).dirname()  + "': " +
        mkdir.error());
  }

  Result<string> endpointDir = os::realpath(symlinkPath);
  if (endpointDir.isSome()) {
    return path::join(endpointDir.get(), ENDPOINT_SOCKET_FILE);
  }

  if (os::exists(symlinkPath)) {
    Try<Nothing> rm = os::rm(symlinkPath);
    if (rm.isError()) {
      return Error(
          "Failed to remove endpoint symlink '" + symlinkPath + "': " +
          rm.error());
    }
  }

  Try<string> mkdtemp = os::mkdtemp(path::join(os::temp(), ENDPOINT_DIR));
  if (mkdtemp.isError()) {
    return Error(
        "Failed to create endpoint directory in '" + os::temp() + "': " +
        mkdtemp.error());
  }

  Try<Nothing> symlink = fs::symlink(mkdtemp.get(), symlinkPath);
  if (symlink.isError()) {
    return Error(
        "Failed to symlink directory '" + mkdtemp.get() + "' to '" +
        symlinkPath + "': " + symlink.error());
  }

  const string socketPath = path::join(mkdtemp.get(), ENDPOINT_SOCKET_FILE);

  // Check if the socket path is too long.
  Try<process::network::unix::Address> address =
    process::network::unix::Address::create(socketPath);
  if (address.isError()) {
    return Error(
        "Failed to create address from '" + socketPath + "': " +
        address.error());
  }

  return socketPath;
#endif // __WINDOWS__
}


Try<list<string>> getVolumePaths(
    const string& rootDir,
    const string& type,
    const string& name)
{
  return fs::list(path::join(rootDir, type, name, VOLUMES_DIR, "*"));
}


string getVolumePath(
    const string& rootDir,
    const string& type,
    const string& name,
    const string& volumeId)
{
  // Volume ID is percent-encoded to avoid invalid characters in the path.
  return path::join(rootDir, type, name, VOLUMES_DIR, http::encode(volumeId));
}


Try<VolumePath> parseVolumePath(const string& rootDir, const string& dir)
{
  // TODO(chhsiao): Consider using `<regex>`, which requires GCC 4.9+.

  // Make sure there's a separator at the end of the `rootDir` so that
  // we don't accidentally slice off part of a directory.
  const string prefix = path::join(rootDir, "");

  if (!strings::startsWith(dir, prefix)) {
    return Error(
        "Directory '" + dir + "' does not fall under the root directory '" +
        rootDir + "'");
  }

  vector<string> tokens = strings::tokenize(
      dir.substr(prefix.size()),
      stringify(os::PATH_SEPARATOR));

  // A complete volume path consists of 4 tokens:
  //   <type>/<name>/volumes/<volume_id>
  if (tokens.size() != 4 || tokens[2] != VOLUMES_DIR) {
    return Error(
        "Path '" + path::join(tokens) + "' does not match the structure of a "
        "volume path");
  }

  // Volume ID is percent-encoded to avoid invalid characters in the path.
  Try<string> volumeId = http::decode(tokens[3]);
  if (volumeId.isError()) {
    return Error(
        "Could not decode volume ID from string '" + tokens[3] + "': " +
        volumeId.error());
  }

  return VolumePath{tokens[0], tokens[1], volumeId.get()};
}


string getVolumeStatePath(
    const string& rootDir,
    const string& type,
    const string& name,
    const string& volumeId)
{
  return path::join(
      getVolumePath(rootDir, type, name, volumeId),
      VOLUME_STATE_FILE);
}


string getMountRootDir(
    const string& rootDir,
    const string& type,
    const string& name)
{
  return path::join(rootDir, type, name, MOUNTS_DIR);
}


Try<list<string>> getMountPaths(const string& mountRootDir)
{
  return fs::list(path::join(mountRootDir, "*"));
}


string getMountPath(const string& mountRootDir, const string& volumeId)
{
  // Volume ID is percent-encoded to avoid invalid characters in the path.
  return path::join(mountRootDir, http::encode(volumeId));
}


Try<string> parseMountPath(const string& mountRootDir, const string& dir)
{
  // TODO(chhsiao): Consider using `<regex>`, which requires GCC 4.9+.

  // Make sure there's a separator at the end of the `mountRootDir` so that we
  // don't accidentally slice off part of a directory.
  const string prefix = path::join(mountRootDir, "");

  if (!strings::startsWith(dir, prefix)) {
    return Error(
        "Directory '" + dir + "' does not fall under the mount root directory"
        " '" + mountRootDir + "'");
  }

  // Volume ID is percent-encoded to avoid invalid characters in the path.
  Try<string> volumeId = http::decode(Path(dir).basename());
  if (volumeId.isError()) {
    return Error(
        "Could not decode volume ID from string '" + Path(dir).basename() +
        "': " + volumeId.error());
  }

  return volumeId.get();
}


string getMountStagingPath(
    const string& mountRootDir,
    const string& volumeId)
{
  return path::join(getMountPath(mountRootDir, volumeId), STAGING_DIR);
}


string getMountTargetPath(
    const string& mountRootDir,
    const string& volumeId)
{
  return path::join(getMountPath(mountRootDir, volumeId), TARGET_DIR);
}

} // namespace paths {
} // namespace csi {
} // namespace mesos {
