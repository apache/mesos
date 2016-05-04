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

#ifndef __ISOLATOR_DOCKER_VOLUME_DRIVER_HPP__
#define __ISOLATOR_DOCKER_VOLUME_DRIVER_HPP__

#include <string>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace volume {

/**
 * Abstraction for Docker Volume Driver Client.
 *
 * TODO(gyliu513): Update the docker volume driver client use docker
 * volume driver API directly via curl.
 *
 * Refer to https://github.com/yp-engineering/rbd-docker-plugin for
 * how `curl` works with docker volume driver API.
 */
class DriverClient
{
public:
  /**
   * Create a Docker Volume Driver Client.
   */
  static Try<process::Owned<DriverClient>> create(
      const std::string& dvdcli);

  virtual ~DriverClient() {}

  /**
   * Performs a 'mount' and returns the path of the mount point. If
   * the 'options' field is specified, the 'mount' will do an implicit
   * creation of the volume if it does not exist.
   */
  virtual process::Future<std::string> mount(
      const std::string& driver,
      const std::string& name,
      const hashmap<std::string, std::string>& options);

  /**
   * Performs an 'unmount'.
   */
  virtual process::Future<Nothing> unmount(
      const std::string& driver,
      const std::string& name);

protected:
  DriverClient() {} // For creating mock object.

private:
  DriverClient(const std::string& _dvdcli)
    : dvdcli(_dvdcli) {}

  const std::string dvdcli;
};

} // namespace volume {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __ISOLATOR_DOCKER_VOLUME_DRIVER_HPP__
