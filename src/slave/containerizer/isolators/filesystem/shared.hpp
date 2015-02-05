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

#ifndef __SHARED_FILESYSTEM_ISOLATOR_HPP__
#define __SHARED_FILESYSTEM_ISOLATOR_HPP__

#include "slave/containerizer/isolator.hpp"

namespace mesos {
namespace slave {

// This isolator is to be used when all containers share the host's
// filesystem.  It supports creating mounting "volumes" from the host
// into each container's mount namespace. In particular, this can be
// used to give each container a "private" system directory, such as
// /tmp and /var/tmp.
class SharedFilesystemIsolatorProcess : public IsolatorProcess
{
public:
  static Try<Isolator*> create(const Flags& flags);

  virtual ~SharedFilesystemIsolatorProcess();

  virtual process::Future<Nothing> recover(
      const std::list<state::RunState>& states);

  virtual process::Future<Option<CommandInfo> > prepare(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user);

  virtual process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid);

  virtual process::Future<Limitation> watch(
      const ContainerID& containerId);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<Nothing> cleanup(
      const ContainerID& containerId);

private:
  SharedFilesystemIsolatorProcess(const Flags& flags);

  const Flags flags;
};

} // namespace slave {
} // namespace mesos {

#endif // __SHARED_FILESYSTEM_ISOLATOR_HPP__
