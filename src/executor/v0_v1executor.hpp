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

#ifndef __V0_V1EXECUTOR_HPP__
#define __V0_V1EXECUTOR_HPP__

#include <functional>
#include <string>

#include <mesos/executor.hpp>
#include <mesos/mesos.hpp>

#include <mesos/v1/executor.hpp>

#include <process/owned.hpp>

namespace mesos {
namespace v1 {
namespace executor {

class V0ToV1AdapterProcess; // Forward declaration.

// This interface acts as an adapter from the v0 (driver + executor) to the
// v1 Mesos executor.
class V0ToV1Adapter : public mesos::Executor, public MesosBase
{
public:
  V0ToV1Adapter(
      const std::function<void(void)>& connected,
      const std::function<void(void)>& disconnected,
      const std::function<void(const std::queue<Event>&)>& received);

  ~V0ToV1Adapter() override;

  void registered(
      ExecutorDriver* driver,
      const mesos::ExecutorInfo& executorInfo,
      const mesos::FrameworkInfo& frameworkInfo,
      const mesos::SlaveInfo& slaveInfo) override;

  void reregistered(
      ExecutorDriver* driver,
      const mesos::SlaveInfo& slaveInfo) override;

  void launchTask(
      ExecutorDriver* driver,
      const mesos::TaskInfo& task) override;

  void disconnected(ExecutorDriver* driver) override;

  void killTask(
      ExecutorDriver* driver,
      const mesos::TaskID& taskId) override;

  void frameworkMessage(
      ExecutorDriver* driver,
      const std::string& data) override;

  void shutdown(ExecutorDriver* driver) override;

  void error(
      ExecutorDriver* driver,
      const std::string& message) override;

  void send(const Call& call) override;

private:
  process::Owned<V0ToV1AdapterProcess> process;
  MesosExecutorDriver driver;
};

} // namespace executor {
} // namespace v1 {
} // namespace mesos {

#endif // __V0_V1EXECUTOR_HPP__
