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

#include <stdio.h>

#include <string>

#include <mesos/mesos.hpp>
#include <mesos/executor.hpp>

#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/subprocess.hpp>
#include <process/reap.hpp>

#include <stout/flags.hpp>
#include <stout/os.hpp>

#include "common/status_utils.hpp"

#include "logging/logging.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::string;

namespace mesos {
namespace internal {

using namespace mesos;
using namespace process;


// Executor that is responsible to execute a docker container, and
// redirect log output to configured stdout and stderr files.
// Similar to the CommandExecutor, it is only responsible to launch
// one container and exits afterwards.
// The executor also assumes it is launched from the
// DockerContainerizer, which already calls setsid before launching
// the executor.
class DockerExecutorProcess : public ProtobufProcess<DockerExecutorProcess>
{
public:
  DockerExecutorProcess(const string& docker, const string& container)
    : launched(false),
      docker(docker),
      container(container),
      pid(-1) {}

  virtual ~DockerExecutorProcess() {}

  void registered(
      ExecutorDriver* _driver,
      const ExecutorInfo& executorInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo)
  {
    cout << "Registered docker executor on " << slaveInfo.hostname() << endl;
    driver = _driver;
  }

  void reregistered(
      ExecutorDriver* driver,
      const SlaveInfo& slaveInfo)
  {
    cout << "Re-registered docker executor on " << slaveInfo.hostname() << endl;
  }

  void disconnected(ExecutorDriver* driver) {}

  void launchTask(ExecutorDriver* driver, const TaskInfo& task)
  {
    if (launched) {
      TaskStatus status;
      status.mutable_task_id()->MergeFrom(task.task_id());
      status.set_state(TASK_FAILED);
      status.set_message(
          "Attempted to run multiple tasks using a \"docker\" executor");

      driver->sendStatusUpdate(status);
      return;
    }

    cout << "Starting task " << task.task_id().value() << endl;

    Try<Subprocess> subprocess = process::subprocess(
        "exit `" + docker + " wait + " + container + "`",
        Subprocess::PATH("/dev/null"),
        Subprocess::FD(STDOUT_FILENO),
        Subprocess::FD(STDERR_FILENO));

    if (subprocess.isError()) {
      cerr << "Couldn't launch docker wait process: " << subprocess.error();
      abort();
    }

    pid = subprocess.get().pid();

    process::reap(pid)
      .onAny(defer(self(),
                   &Self::reaped,
                   driver,
                   task.task_id(),
                   pid,
                   lambda::_1));

    TaskStatus status;
    status.mutable_task_id()->MergeFrom(task.task_id());
    status.set_state(TASK_RUNNING);
    driver->sendStatusUpdate(status);

    launched = true;
  }

  void killTask(ExecutorDriver* driver, const TaskID& taskId)
  {
    shutdown(driver);
  }

  void frameworkMessage(ExecutorDriver* driver, const string& data) {}

  void shutdown(ExecutorDriver* driver)
  {
    cout << "Shutting down" << endl;

    if (pid > 0 && !killed) {
      ::kill(pid, SIGKILL);
      killed = true;
    }
  }

  virtual void error(ExecutorDriver* driver, const string& message) {}

private:
  void reaped(
      ExecutorDriver* driver,
      const TaskID& taskId,
      pid_t pid,
      const Future<Option<int>>& status)
  {
    TaskState state;
    string message;
    if (!status.isReady()) {
      state = TASK_FAILED;
      message =
        "Failed to get exit status for Docker executor: " +
        (status.isFailed() ? status.failure() : "future discarded");
    } else if (status.get().isNone()) {
      state = TASK_FAILED;
      message = "Failed to get exit status for Docker executor";
    } else {
      int s = status.get().get();

      // Subprocess status is gathered from waitpid, therefore we can
      // get the exit status from WIFEXITED.
      CHECK(WIFEXITED(s) || WIFSIGNALED(s)) << "status code: " << s;

      if (WIFEXITED(s) && WEXITSTATUS(s) == 0) {
        state = TASK_FINISHED;
      } else if (killed) {
        // Send TASK_KILLED if the task was killed as a result of
        // killTask() or shutdown().
        state = TASK_KILLED;
      } else {
        state = TASK_FAILED;
      }

      message = "Docker  " + WSTRINGIFY(s);
    }

    cout << message << " (pid: " << pid << ")" << endl;

    TaskStatus taskStatus;
    taskStatus.mutable_task_id()->MergeFrom(taskId);
    taskStatus.set_state(state);
    taskStatus.set_message(message);

    driver->sendStatusUpdate(taskStatus);

    // A hack for now ... but we need to wait until the status update
    // is sent to the slave before we shut ourselves down.
    os::sleep(Seconds(1));
    driver->stop();
  }


  bool launched;
  string docker;
  string container;
  pid_t pid;
  bool killed;
  Option<ExecutorDriver*> driver;
};


class DockerExecutor : public Executor
{
public:
  DockerExecutor(const string& docker, const string& container)
  {
    process = new DockerExecutorProcess(docker, container);
    spawn(process);
  }

  virtual ~DockerExecutor()
  {
    terminate(process);
    wait(process);
    delete process;
  }

  virtual void registered(
      ExecutorDriver* driver,
      const ExecutorInfo& executorInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo)
  {
    dispatch(process,
             &DockerExecutorProcess::registered,
             driver,
             executorInfo,
             frameworkInfo,
             slaveInfo);
  }

  virtual void reregistered(
      ExecutorDriver* driver,
      const SlaveInfo& slaveInfo)
  {
    dispatch(process,
             &DockerExecutorProcess::reregistered,
             driver,
             slaveInfo);
  }

  virtual void disconnected(ExecutorDriver* driver)
  {
    dispatch(process, &DockerExecutorProcess::disconnected, driver);
  }

  virtual void launchTask(ExecutorDriver* driver, const TaskInfo& task)
  {
    dispatch(process, &DockerExecutorProcess::launchTask, driver, task);
  }

  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId)
  {
    dispatch(process, &DockerExecutorProcess::killTask, driver, taskId);
  }

  virtual void frameworkMessage(ExecutorDriver* driver, const string& data)
  {
    dispatch(process, &DockerExecutorProcess::frameworkMessage, driver, data);
  }

  virtual void shutdown(ExecutorDriver* driver)
  {
    dispatch(process, &DockerExecutorProcess::shutdown, driver);
  }

  virtual void error(ExecutorDriver* driver, const string& data)
  {
    dispatch(process, &DockerExecutorProcess::error, driver, data);
  }

private:
  DockerExecutorProcess* process;
};

} // namespace internal {
} // namespace mesos {


void usage(const char* argv0, const flags::FlagsBase& flags)
{
  cerr << "Usage: " << os::basename(argv0).get() << " [...]" << endl
       << endl
       << "Supported options:" << endl
       << flags.usage();
}


class Flags : public flags::FlagsBase
{
public:
  Flags()
  {
    add(&Flags::container,
        "container",
        "The name of the docker container to wait on.");

    add(&Flags::docker,
        "docker",
        "The path to the docker cli executable.");
  }

  Option<string> container;
  Option<string> docker;
};


int main(int argc, char** argv)
{
  Flags flags;

  bool help;
  flags.add(&help,
            "help",
            "Prints this help message",
            false);

  // Load flags from environment and command line.
  Try<Nothing> load = flags.load(None(), &argc, &argv);

  if (load.isError()) {
    cerr << load.error() << endl;
    usage(argv[0], flags);
    return -1;
  }

  if (help) {
    usage(argv[0], flags);
    return -1;
  }

  if (flags.docker.isNone()) {
    LOG(WARNING) << "Expected docker executable path";
    usage(argv[0], flags);
    return 0;
  }

  if (flags.container.isNone()) {
    LOG(WARNING) << "Expected container name";
    usage(argv[0], flags);
    return 0;
  }

  mesos::internal::DockerExecutor executor(
      flags.docker.get(), flags.container.get());
  mesos::MesosExecutorDriver driver(&executor);
  return driver.run() == mesos::DRIVER_STOPPED ? 0 : 1;
}
