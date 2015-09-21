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

#ifndef __PROVISIONER_DOCKER_LOCAL_PULLER_HPP__
#define __PROVISIONER_DOCKER_LOCAL_PULLER_HPP__

#include "slave/containerizer/provisioner/store.hpp"

#include "slave/containerizer/provisioner/docker/message.hpp"
#include "slave/containerizer/provisioner/docker/puller.hpp"

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward declaration.
class LocalPullerProcess;


/**
 * LocalPuller assumes Docker images are stored in a local directory
 * (configured with flags.docker_local_archives_dir), with all the
 * images saved as tars with file names in the form of <repo>:<tag>.tar.
 */
class LocalPuller : public Puller
{
public:
  explicit LocalPuller(const Flags& flags);

  ~LocalPuller();

  process::Future<std::list<std::pair<std::string, std::string>>> pull(
      const Image::Name& name,
      const std::string& directory);

private:
  LocalPuller& operator=(const LocalPuller&) = delete; // Not assignable.
  LocalPuller(const LocalPuller&) = delete; // Not copyable.

  process::Owned<LocalPullerProcess> process;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_DOCKER_LOCAL_PULLER_HPP__
