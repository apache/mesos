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
#include <string>

#include <mesos/executor.hpp>

#include <stout/duration.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>

#include "common/process_utils.hpp"
#include "common/thread.hpp"

#include "logging/logging.hpp"

using std::string;

namespace mesos {
namespace internal {

// Waits for command to finish. Note that we currently launch a thread
// that calls this function and thus the ExecutorDriver pointer might
// be used by multiple threads. This is not ideal, but should be
// sufficient for now (see the comment below where we instantiate the
// MesosSchedulerDriver for how we "get around" any issues related to
// the driver pointer becoming a dangling reference).
static void waiter(pid_t pid, const TaskID& taskId, ExecutorDriver* driver)
{
  int status;
  while (wait(&status) != pid || WIFSTOPPED(status));

  CHECK(WIFEXITED(status) || WIFSIGNALED(status));

  std::cout << "Waited on process " << pid
            << ", returned status " << status << std::endl;

  TaskStatus taskStatus;
  taskStatus.mutable_task_id()->MergeFrom(taskId);

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    taskStatus.set_state(TASK_FINISHED);
  } else {
    taskStatus.set_state(TASK_FAILED);
  }

  Try<string> message = WIFEXITED(status)
    ? strings::format("Command exited with status %d", WEXITSTATUS(status))
    : strings::format("Command terminated with signal '%s'",
                      strsignal(WTERMSIG(status)));

  if (message.isSome()) {
    taskStatus.set_message(message.get());
  }

  driver->sendStatusUpdate(taskStatus);

  // A hack for now ... but we need to wait until for the status
  // update to get sent to the slave before we shut ourselves down.
  os::sleep(Seconds(1));
  driver->stop();
}


class CommandExecutor : public Executor
{
public:
  CommandExecutor()
    : launched(false),
      pid(-1) {}

  virtual ~CommandExecutor() {}

  virtual void registered(
      ExecutorDriver* driver,
      const ExecutorInfo& executorInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo)
  {
    std::cout << "Registered executor on " << slaveInfo.hostname() << std::endl;
  }

  virtual void reregistered(ExecutorDriver* driver,
                            const SlaveInfo& slaveInfo)
  {
    std::cout << "Re-registered executor on " << slaveInfo.hostname()
      << std::endl;
  }

  virtual void disconnected(ExecutorDriver* driver) {}

  virtual void launchTask(ExecutorDriver* driver, const TaskInfo& task)
  {
    if (launched) {
      TaskStatus status;
      status.mutable_task_id()->MergeFrom(task.task_id());
      status.set_state(TASK_FAILED);
      status.set_message("Attempted to run tasks using a \"command\" executor");
      driver->sendStatusUpdate(status);
      return;
    }

    CHECK(task.has_command()) << "Expecting task to have a command!";

    std::cout << "Starting task " << task.task_id().value() << std::endl;

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
      close(pipes[0]);

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

      close(pipes[1]);

      // The child has successfully setsid, now run the command.
      std::cout << "sh -c '" << task.command().value() << "'" << std::endl;
      execl("/bin/sh", "sh", "-c",
            task.command().value().c_str(), (char*) NULL);
      perror("Failed to exec");
      abort();
    }

    // In parent process.
    close(pipes[1]);

    // Get the child's pid via the pipe.
    if (read(pipes[0], &pid, sizeof(pid)) == -1) {
      std::cerr << "Failed to get child PID from pipe, read: "
                << strerror(errno) << std::endl;
      abort();
    }

    close(pipes[0]);

    std::cout << "Forked command at " << pid << std::endl;

    // In parent process, fork a thread to wait for this process.
    thread::start(std::tr1::bind(&waiter, pid, task.task_id(), driver));

    TaskStatus status;
    status.mutable_task_id()->MergeFrom(task.task_id());
    status.set_state(TASK_RUNNING);
    driver->sendStatusUpdate(status);

    launched = true;
  }

  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId)
  {
    // TODO(benh): Do kill escalation (i.e., after n seconds, kill -9).
    if (pid > 0) {
      utils::process::killtree(pid, SIGTERM, true, true, true);
    }
  }

  virtual void frameworkMessage(ExecutorDriver* driver, const string& data) {}

  virtual void shutdown(ExecutorDriver* driver)
  {
    // TODO(benh): Do kill escalation (i.e., after n seconds, kill -9).
    if (pid > 0) {
      utils::process::killtree(pid, SIGTERM, true, true, true);
    }
  }

  virtual void error(ExecutorDriver* driver, const string& message) {}

private:
  bool launched;
  pid_t pid;
};

} // namespace internal {
} // namespace mesos {


int main(int argc, char** argv)
{
  mesos::internal::CommandExecutor executor;

  // Note that we currently put the MesosSchedulerDriver on the heap
  // so that we don't have to deal with issues created because the
  // thread we launched is trying to use the pointer.
  mesos::MesosExecutorDriver* driver =
    new mesos::MesosExecutorDriver(&executor);

  return driver->run() == mesos::DRIVER_STOPPED ? 0 : 1;
}
