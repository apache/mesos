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

#ifndef __ISOLATOR_HPP__
#define __ISOLATOR_HPP__

#include <unistd.h>

#include <string>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/uuid.hpp>

#include "common/resources.hpp"

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace state {

class SlaveState; // Forward declaration.

} // namespace state {

// Forward declaration.
class Slave;


class Isolator : public process::Process<Isolator>
{
public:
  static Isolator* create(const std::string& type);
  static void destroy(Isolator* isolator);

  virtual ~Isolator() {}

  // Called during slave initialization.
  virtual void initialize(
      const Flags& flags,
      const Resources& resources,
      bool local,
      const process::PID<Slave>& slave) = 0;

  // Called by the slave to launch an executor for a given framework.
  // If 'checkpoint' is true, the isolator is expected to checkpoint
  // the executor pid to the 'path'.
  virtual void launchExecutor(
      const SlaveID& slaveId, // TODO(vinod): Why not pass this to initialize?
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const UUID& uuid,
      const std::string& directory,
      const Resources& resources) = 0;

  // Terminate a framework's executor, if it is still running.
  // The executor is expected to be gone after this method exits.
  virtual void killExecutor(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId) = 0;

  // Update the resource limits for a given framework. This method will
  // be called only after an executor for the framework is started.
  virtual void resourcesChanged(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const Resources& resources) = 0;

  // Returns the resource usage for the isolator.
  virtual process::Future<ResourceStatistics> usage(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId) = 0;

  // Recover executors.
  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state) = 0;
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __ISOLATOR_HPP__
