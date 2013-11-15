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

#include <signal.h>
#include <stdio.h>

#include <sys/wait.h>

#include <iostream>
#include <list>
#include <string>

#include <mesos/executor.hpp>

#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>

#include "common/type_utils.hpp"

#include "logging/logging.hpp"

#include "slave/reaper.hpp"

using process::wait; // Necessary on some OS's to disambiguate.

using std::cout;
using std::cerr;
using std::endl;
using std::string;

namespace mesos {
namespace internal {

using namespace process;


class CommandExecutorProcess : public Process<CommandExecutorProcess>
{
public:
  CommandExecutorProcess()
    : launched(false),
      killed(false),
      pid(-1) {}

  virtual ~CommandExecutorProcess() {}

  void registered(
      ExecutorDriver* driver,
      const ExecutorInfo& executorInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo)
  {
    cout << "Registered executor on " << slaveInfo.hostname() << endl;
  }

  void reregistered(
      ExecutorDriver* driver,
      const SlaveInfo& slaveInfo)
  {
    cout << "Re-registered executor on " << slaveInfo.hostname() << endl;
  }

  void disconnected(ExecutorDriver* driver) {}

  void launchTask(ExecutorDriver* driver, const TaskInfo& task)
  {
    if (launched) {
      TaskStatus status;
      status.mutable_task_id()->MergeFrom(task.task_id());
      status.set_state(TASK_FAILED);
      status.set_message(
          "Attempted to run multiple tasks using a \"command\" executor");

      driver->sendStatusUpdate(status);
      return;
    }

    CHECK(task.has_command()) << "Expecting task " << task.task_id()
                              << " to have a command!";

    std::cout << "Starting task " << task.task_id() << std::endl;

    // TODO(benh): Clean this up with the new 'Fork' abstraction.
    // Use pipes to determine which child has successfully changed
    // session. This is needed as the setsid call can fail from other
    // processes having the same group id.
    int pipes[2];
    if (pipe(pipes) < 0) {
      perror("Failed to create a pipe");
      abort();
    }

    // Set the FD_CLOEXEC flags on these pipes
    Try<Nothing> cloexec = os::cloexec(pipes[0]);
    if (cloexec.isError()) {
      std::cerr << "Failed to cloexec(pipe[0]): " << cloexec.error()
                << std::endl;
      abort();
    }

    cloexec = os::cloexec(pipes[1]);
    if (cloexec.isError()) {
      std::cerr << "Failed to cloexec(pipe[1]): " << cloexec.error()
                << std::endl;
      abort();
    }

    if ((pid = fork()) == -1) {
      std::cerr << "Failed to fork to run '" << task.command().value() << "': "
                << strerror(errno) << std::endl;
      abort();
    }

    if (pid == 0) {
      // In child process, we make cleanup easier by putting process
      // into it's own session.
      os::close(pipes[0]);

      // NOTE: We setsid() in a loop because setsid() might fail if another
      // process has the same process group id as the calling process.
      while ((pid = setsid()) == -1) {
        perror("Could not put command in its own session, setsid");

        std::cout << "Forking another process and retrying" << std::endl;

        if ((pid = fork()) == -1) {
          perror("Failed to fork to launch command");
          abort();
        }

        if (pid > 0) {
          // In parent process. It is ok to suicide here, because
          // we're not watching this process.
          exit(0);
        }
      }

      if (write(pipes[1], &pid, sizeof(pid)) != sizeof(pid)) {
        perror("Failed to write PID on pipe");
        abort();
      }

      os::close(pipes[1]);

      // The child has successfully setsid, now run the command.
      std::cout << "sh -c '" << task.command().value() << "'" << std::endl;
      execl("/bin/sh", "sh", "-c",
            task.command().value().c_str(), (char*) NULL);
      perror("Failed to exec");
      abort();
    }

    // In parent process.
    os::close(pipes[1]);

    // Get the child's pid via the pipe.
    if (read(pipes[0], &pid, sizeof(pid)) == -1) {
      std::cerr << "Failed to get child PID from pipe, read: "
                << strerror(errno) << std::endl;
      abort();
    }

    os::close(pipes[0]);

    std::cout << "Forked command at " << pid << std::endl;

    // Monitor this process.
    reaper.monitor(pid)
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
    std::cout << "Shutting down" << std::endl;

    // TODO(benh): Do kill escalation (begin with SIGTERM, after n
    // seconds escalate to SIGKILL).
    if (pid > 0 && !killed) {
      std::cout << "Killing process tree at pid " << pid << std::endl;

      Try<std::list<os::ProcessTree> > trees =
        os::killtree(pid, SIGKILL, true, true);

      if (trees.isError()) {
        std::cerr << "Failed to kill the process tree rooted at pid "
                  << pid << ": " << trees.error() << std::endl;
      } else {
        std::cout << "Killed the following process trees:\n"
                  << stringify(trees.get()) << std::endl;
      }

      killed = true;
    }
  }

  virtual void error(ExecutorDriver* driver, const string& message) {}

private:
  void reaped(
      ExecutorDriver* driver,
      const TaskID& taskId,
      pid_t pid,
      const Future<Option<int> >& status_)
  {
    TaskState state;
    string message;

    if (!status_.isReady()) {
      state = TASK_FAILED;
      message =
        "Failed to get exit status for Command: " +
        (status_.isFailed() ? status_.failure() : "future discarded");
    } else if (status_.get().isNone()) {
      state = TASK_FAILED;
      message = "Failed to get exit status for Command";
    } else {
      int status = status_.get().get();
      CHECK(WIFEXITED(status) || WIFSIGNALED(status)) << status;

      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        state = TASK_FINISHED;
      } else if (killed) {
        // Send TASK_KILLED if the task was killed as a result of
        // killTask() or shutdown().
        state = TASK_KILLED;
      } else {
        state = TASK_FAILED;
      }

      message = string("Command") +
          (WIFEXITED(status)
          ? " exited with status "
          : " terminated with signal ") +
          (WIFEXITED(status)
          ? stringify(WEXITSTATUS(status))
          : strsignal(WTERMSIG(status)));
    }

    cout << message << " (pid: " << pid << ")" << endl;

    TaskStatus taskStatus;
    taskStatus.mutable_task_id()->MergeFrom(taskId);
    taskStatus.set_state(state);
    taskStatus.set_message(message);

    driver->sendStatusUpdate(taskStatus);
    driver->stop();
  }

  bool launched;
  bool killed;
  pid_t pid;
  slave::Reaper reaper;
};


class CommandExecutor: public Executor
{
public:
  CommandExecutor()
  {
    process = new CommandExecutorProcess();
    spawn(process);
  }

  virtual ~CommandExecutor()
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
             &CommandExecutorProcess::registered,
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
             &CommandExecutorProcess::reregistered,
             driver,
             slaveInfo);
  }

  virtual void disconnected(ExecutorDriver* driver)
  {
    dispatch(process, &CommandExecutorProcess::disconnected, driver);
  }

  virtual void launchTask(ExecutorDriver* driver, const TaskInfo& task)
  {
    dispatch(process, &CommandExecutorProcess::launchTask, driver, task);
  }

  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId)
  {
    dispatch(process, &CommandExecutorProcess::killTask, driver, taskId);
  }

  virtual void frameworkMessage(ExecutorDriver* driver, const string& data)
  {
    dispatch(process, &CommandExecutorProcess::frameworkMessage, driver, data);
  }

  virtual void shutdown(ExecutorDriver* driver)
  {
    dispatch(process, &CommandExecutorProcess::shutdown, driver);
  }

  virtual void error(ExecutorDriver* driver, const string& data)
  {
    dispatch(process, &CommandExecutorProcess::error, driver, data);
  }

private:
  CommandExecutorProcess* process;
};

} // namespace internal {
} // namespace mesos {


int main(int argc, char** argv)
{
  mesos::internal::CommandExecutor executor;
  mesos::MesosExecutorDriver driver(&executor);
  return driver.run() == mesos::DRIVER_STOPPED ? 0 : 1;
}
