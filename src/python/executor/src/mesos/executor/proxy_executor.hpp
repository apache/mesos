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

#ifndef PROXY_EXECUTOR_HPP
#define PROXY_EXECUTOR_HPP

// Python.h must be included before standard headers.
// See: http://docs.python.org/2/c-api/intro.html#include-files
#include <Python.h>

#include <string>
#include <vector>

#include <mesos/executor.hpp>

namespace mesos {
namespace python {

struct MesosExecutorDriverImpl;

/**
 * Proxy Executor implementation that will call into Python.
 */
class ProxyExecutor : public Executor
{
public:
  explicit ProxyExecutor(MesosExecutorDriverImpl *_impl) : impl(_impl) {}

  virtual ~ProxyExecutor() {}

  virtual void registered(ExecutorDriver* driver,
                          const ExecutorInfo& executorInfo,
                          const FrameworkInfo& frameworkInfo,
                          const SlaveInfo& slaveInfo);
  virtual void reregistered(ExecutorDriver* driver, const SlaveInfo& slaveInfo);
  virtual void disconnected(ExecutorDriver* driver);
  virtual void launchTask(ExecutorDriver* driver, const TaskInfo& task);
  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId);
  virtual void frameworkMessage(ExecutorDriver* driver,
                                const std::string& data);
  virtual void shutdown(ExecutorDriver* driver);
  virtual void error(ExecutorDriver* driver, const std::string& message);

private:
  MesosExecutorDriverImpl *impl;
};

} // namespace python {
} // namespace mesos {

#endif // PROXY_EXECUTOR_HPP
