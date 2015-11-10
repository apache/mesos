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

#include <vector>

#include <process/subprocess.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/mesos/provisioner/docker/puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/local_puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/registry_puller.hpp"

using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Subprocess;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

Try<Owned<Puller>> Puller::create(const Flags& flags)
{
  const string puller = flags.docker_puller;

  if (puller == "local") {
    return Owned<Puller>(new LocalPuller(flags));
  }

  if (puller == "registry") {
    Try<Owned<Puller>> puller = RegistryPuller::create(flags);
    if (puller.isError()) {
      return Error("Failed to create registry puller: " + puller.error());
    }

    return puller.get();
  }

  return Error("Unknown or unsupported docker puller: " + puller);
}


Future<Nothing> untar(const string& file, const string& directory)
{
  const vector<string> argv = {
    "tar",
    "-C",
    directory,
    "-x",
    "-f",
    file
  };

  Try<Subprocess> s = subprocess(
      "tar",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"));

  if (s.isError()) {
    return Failure(
        "Failed to create untar subprocess for file '" +
        file + "': " + s.error());
  }

  return s.get().status()
    .then([file](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure(
            "Failed to reap untar subprocess for file '" + file + "'");
      }

      if (!WIFEXITED(status.get()) ||
          WEXITSTATUS(status.get()) != 0) {
        return Failure(
            "Untar process for file '" + file + "' failed with exit code: " +
            WSTRINGIFY(status.get()));
      }

      return Nothing();
    });
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
