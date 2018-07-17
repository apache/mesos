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

#ifndef __MESOS_EXECUTOR_HPP__
#define __MESOS_EXECUTOR_HPP__

#include <map>
#include <mutex>
#include <string>

#include <mesos/mesos.hpp>

// Mesos executor interface and executor driver. An executor is
// responsible for launching tasks in a framework specific way (i.e.,
// creating new threads, new processes, etc). One or more executors
// from the same framework may run concurrently on the same machine.
// Note that we use the term "executor" fairly loosely to refer to the
// code that implements the Executor interface (see below) as well as
// the program that is responsible for instantiating a new
// MesosExecutorDriver (also below). In fact, while a Mesos slave is
// responsible for (forking and) executing the "executor", there is no
// reason why whatever the slave executed might itself actually
// execute another program which actually instantiates and runs the
// MesosSchedulerDriver. The only contract with the slave is that the
// program that it invokes does not exit until the "executor" has
// completed. Thus, what the slave executes may be nothing more than a
// script which actually executes (or forks and waits) the "real"
// executor.
//
// IF YOU FIND YOURSELF MODIFYING COMMENTS HERE PLEASE CONSIDER MAKING
// THE SAME MODIFICATIONS FOR OTHER LANGUAGE BINDINGS (e.g., Java:
// src/java/src/org/apache/mesos, Python: src/python/src, etc.).

// Forward declaration.
namespace process {
class Latch;
} // namespace process {

namespace mesos {

// A few forward declarations.
class ExecutorDriver;

namespace internal {
class ExecutorProcess;
}

// Callback interface to be implemented by frameworks' executors. Note
// that only one callback will be invoked at a time, so it is not
// recommended that you block within a callback because it may cause a
// deadlock.
//
// Each callback includes a pointer to the executor driver that was
// used to run this executor. The pointer will not change for the
// duration of an executor (i.e., from the point you do
// ExecutorDriver::start() to the point that ExecutorDriver::join()
// returns). This is intended for convenience so that an executor
// doesn't need to store a pointer to the driver itself.
//
// TODO(bmahler): Consider adding a usage() callback here, that
// provides information to the executor about its ResourceUsage.
class Executor
{
public:
  // Empty virtual destructor (necessary to instantiate subclasses).
  virtual ~Executor() {}

  // Invoked once the executor driver has been able to successfully
  // connect with Mesos. In particular, a scheduler can pass some
  // data to its executors through the FrameworkInfo.ExecutorInfo's
  // data field.
  virtual void registered(
      ExecutorDriver* driver,
      const ExecutorInfo& executorInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo) = 0;

  // Invoked when the executor reregisters with a restarted slave.
  virtual void reregistered(
      ExecutorDriver* driver,
      const SlaveInfo& slaveInfo) = 0;

  // Invoked when the executor becomes "disconnected" from the slave
  // (e.g., the slave is being restarted due to an upgrade).
  virtual void disconnected(ExecutorDriver* driver) = 0;

  // Invoked when a task has been launched on this executor (initiated
  // via Scheduler::launchTasks). Note that this task can be realized
  // with a thread, a process, or some simple computation, however, no
  // other callbacks will be invoked on this executor until this
  // callback has returned.
  virtual void launchTask(
      ExecutorDriver* driver,
      const TaskInfo& task) = 0;

  // Invoked when a task running within this executor has been killed
  // (via SchedulerDriver::killTask). Note that no status update will
  // be sent on behalf of the executor, the executor is responsible
  // for creating a new TaskStatus (i.e., with TASK_KILLED) and
  // invoking ExecutorDriver::sendStatusUpdate.
  virtual void killTask(
      ExecutorDriver* driver,
      const TaskID& taskId) = 0;

  // Invoked when a framework message has arrived for this executor.
  // These messages are best effort; do not expect a framework message
  // to be retransmitted in any reliable fashion.
  virtual void frameworkMessage(
      ExecutorDriver* driver,
      const std::string& data) = 0;

  // Invoked when the executor should terminate all of its currently
  // running tasks. Note that after a Mesos has determined that an
  // executor has terminated any tasks that the executor did not send
  // terminal status updates for (e.g., TASK_KILLED, TASK_FINISHED,
  // TASK_FAILED, etc) a TASK_LOST status update will be created.
  virtual void shutdown(ExecutorDriver* driver) = 0;

  // Invoked when a fatal error has occurred with the executor and/or
  // executor driver. The driver will be aborted BEFORE invoking this
  // callback.
  virtual void error(
      ExecutorDriver* driver,
      const std::string& message) = 0;
};


// Abstract interface for connecting an executor to Mesos. This
// interface is used both to manage the executor's lifecycle (start
// it, stop it, or wait for it to finish) and to interact with Mesos
// (e.g., send status updates, send framework messages, etc.). See
// MesosExecutorDriver below for a concrete example of an
// ExecutorDriver.
class ExecutorDriver
{
public:
  // Empty virtual destructor (necessary to instantiate subclasses).
  virtual ~ExecutorDriver() {}

  // Starts the executor driver. This needs to be called before any
  // other driver calls are made.
  virtual Status start() = 0;

  // Stops the executor driver.
  virtual Status stop() = 0;

  // Aborts the driver so that no more callbacks can be made to the
  // executor. The semantics of abort and stop have deliberately been
  // separated so that code can detect an aborted driver (i.e., via
  // the return status of ExecutorDriver::join, see below), and
  // instantiate and start another driver if desired (from within the
  // same process ... although this functionality is currently not
  // supported for executors).
  virtual Status abort() = 0;

  // Waits for the driver to be stopped or aborted, possibly
  // _blocking_ the current thread indefinitely. The return status of
  // this function can be used to determine if the driver was aborted
  // (see mesos.proto for a description of Status).
  virtual Status join() = 0;

  // Starts and immediately joins (i.e., blocks on) the driver.
  virtual Status run() = 0;

  // Sends a status update to the framework scheduler, retrying as
  // necessary until an acknowledgement has been received or the
  // executor is terminated (in which case, a TASK_LOST status update
  // will be sent). See Scheduler::statusUpdate for more information
  // about status update acknowledgements.
  virtual Status sendStatusUpdate(const TaskStatus& status) = 0;

  // Sends a message to the framework scheduler. These messages are
  // best effort; do not expect a framework message to be
  // retransmitted in any reliable fashion.
  virtual Status sendFrameworkMessage(const std::string& data) = 0;
};


// Concrete implementation of an ExecutorDriver that connects an
// Executor with a Mesos slave. The MesosExecutorDriver is
// thread-safe.
//
// The driver is responsible for invoking the Executor callbacks as it
// communicates with the Mesos slave.
//
// Note that blocking on the MesosExecutorDriver (e.g., via
// MesosExecutorDriver::join) doesn't affect the executor callbacks in
// anyway because they are handled by a different thread.
//
// Note that the driver uses GLOG to do its own logging. GLOG flags
// can be set via environment variables, prefixing the flag name with
// "GLOG_", e.g., "GLOG_v=1". For Mesos specific logging flags see
// src/logging/flags.hpp. Mesos flags can also be set via environment
// variables, prefixing the flag name with "MESOS_", e.g.,
// "MESOS_QUIET=1".
//
// See src/examples/test_executor.cpp for an example of using the
// MesosExecutorDriver.
class MesosExecutorDriver : public ExecutorDriver
{
public:
  // Creates a new driver that uses the specified Executor. Note, the
  // executor pointer must outlive the driver.
  //
  // Note that the other constructor overload that accepts `environment`
  // argument is preferable to this one in a multithreaded environment,
  // because the implementation of this one accesses global environment
  // which is unsafe due to a potential concurrent modification of the
  // environment by another thread.
  explicit MesosExecutorDriver(Executor* executor);

  // Creates a new driver that uses the specified `Executor` and environment
  // variables. Note, the executor pointer must outlive the driver.
  explicit MesosExecutorDriver(
      Executor* executor,
      const std::map<std::string, std::string>& environment);

  // This destructor will block indefinitely if
  // MesosExecutorDriver::start was invoked successfully (possibly via
  // MesosExecutorDriver::run) and MesosExecutorDriver::stop has not
  // been invoked.
  ~MesosExecutorDriver() override;

  // See ExecutorDriver for descriptions of these.
  Status start() override;
  Status stop() override;
  Status abort() override;
  Status join() override;
  Status run() override;
  Status sendStatusUpdate(const TaskStatus& status) override;
  Status sendFrameworkMessage(const std::string& data) override;

private:
  friend class internal::ExecutorProcess;

  Executor* executor;

  // Libprocess process for communicating with slave.
  internal::ExecutorProcess* process;

  // Mutex for enforcing serial execution of all non-callbacks.
  std::recursive_mutex mutex;

  // Latch for waiting until driver terminates.
  process::Latch* latch;

  // Current status of the driver.
  Status status;

  std::map<std::string, std::string> environment;
};

} // namespace mesos {

#endif // __MESOS_EXECUTOR_HPP__
