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

#ifndef __MESOS_EXECUTOR_HPP__
#define __MESOS_EXECUTOR_HPP__

#include <string>

#include <mesos/mesos.hpp>


namespace mesos {

class ExecutorDriver;

namespace internal { class ExecutorProcess; }


/**
 * Callback interface to be implemented by frameworks' executors.
 */
class Executor
{
public:
  virtual ~Executor() {}

  virtual void init(ExecutorDriver* driver, const ExecutorArgs& args) = 0;

  virtual void launchTask(ExecutorDriver* driver,
                          const TaskDescription& task) = 0;

  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId) = 0;

  virtual void frameworkMessage(ExecutorDriver* driver,
				const std::string& data) = 0;

  virtual void shutdown(ExecutorDriver* driver) = 0;

  virtual void error(ExecutorDriver* driver,
                     int code,
                     const std::string& message) = 0;
};


/**
 * Abstract interface for driving an executor connected to Mesos.
 * This interface is used both to start the executor running (and
 * communicating with the slave) and to send information from the executor
 * to Mesos (such as status updates). Concrete implementations of
 * ExecutorDriver will take a Executor as a parameter in order to make
 * callbacks into it on various events.
 */
class ExecutorDriver
{
public:
  virtual ~ExecutorDriver() {}

  // Lifecycle methods.
  virtual Status start() = 0;
  virtual Status stop(bool failover = false) = 0;
  virtual Status abort() = 0;
  virtual Status join() = 0;
  virtual Status run() = 0; // Start and then join driver.

  // Communication methods from executor to Mesos.
  virtual Status sendStatusUpdate(const TaskStatus& status) = 0;

  virtual Status sendFrameworkMessage(const std::string& data) = 0;
};


/**
 * Concrete implementation of ExecutorDriver that communicates with a
 * Mesos slave. The slave's location is read from environment variables
 * set by it when it execs the user's executor script; users only need
 * to create the MesosExecutorDriver and call run() on it.
 */
class MesosExecutorDriver : public ExecutorDriver
{
public:
  MesosExecutorDriver(Executor* executor);
  virtual ~MesosExecutorDriver();

  // Lifecycle methods
  virtual Status start();
  virtual Status stop(bool failover = false);
  virtual Status abort();
  virtual Status join();
  virtual Status run(); // Start and then join driver

  virtual Status sendStatusUpdate(const TaskStatus& status);
  virtual Status sendFrameworkMessage(const std::string& data);

private:
  friend class internal::ExecutorProcess;

  Executor* executor;

  // Libprocess process for communicating with slave
  internal::ExecutorProcess* process;

  // Mutex to enforce all non-callbacks are execute serially
  pthread_mutex_t mutex;

  // Condition variable for waiting until driver terminates
  pthread_cond_t cond;

  enum State {
    INITIALIZED,
    RUNNING,
    STOPPED,
    ABORTED
  };

  // Variable to store the state of the driver.
  State state;
};

} // namespace mesos {

#endif // __MESOS_EXECUTOR_HPP__
