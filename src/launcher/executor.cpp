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
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/type_utils.hpp>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/subprocess.hpp>
#include <process/reap.hpp>
#include <process/timer.hpp>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>

#include "common/http.hpp"
#include "common/status_utils.hpp"

#include "logging/logging.hpp"

#include "messages/messages.hpp"

#include "slave/constants.hpp"
#include "slave/graceful_shutdown.hpp"

using process::wait; // Necessary on some OS's to disambiguate.
using namespace mesos::slave;

using std::cout;
using std::cerr;
using std::endl;
using std::string;
using std::vector;

namespace mesos {

using namespace process;

class CommandExecutorProcess : public ProtobufProcess<CommandExecutorProcess>
{
public:
  CommandExecutorProcess(
      Option<char**> override,
      const string& healthCheckDir,
      const Duration& shutdownTimeout)
    : launched(false),
      killed(false),
      killedByHealthCheck(false),
      pid(-1),
      healthPid(-1),
      shutdownTimeout(shutdownTimeout),
      driver(None()),
      healthCheckDir(healthCheckDir),
      override(override) {}

  virtual ~CommandExecutorProcess() {}

  void registered(
      ExecutorDriver* _driver,
      const ExecutorInfo& executorInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo)
  {
    cout << "Registered executor on " << slaveInfo.hostname() << endl;
    driver = _driver;
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

    // Skip sanity checks for TaskInfo if override is provided since
    // the executor will be running the override command.
    if (override.isNone()) {
      // Sanity checks.
      CHECK(task.has_command()) << "Expecting task " << task.task_id()
                                << " to have a command!";

      // TODO(jieyu): For now, we just fail the executor if the task's
      // CommandInfo is not valid. The framework will receive
      // TASK_FAILED for the task, and will most likely find out the
      // cause with some debugging. This is a temporary solution. A more
      // correct solution is to perform this validation at master side.
      if (task.command().shell()) {
        CHECK(task.command().has_value())
          << "Shell command of task " << task.task_id()
          << " is not specified!";
      } else {
        CHECK(task.command().has_value())
          << "Executable of task " << task.task_id()
          << " is not specified!";
      }
    }

    cout << "Starting task " << task.task_id() << endl;

    // TODO(benh): Clean this up with the new 'Fork' abstraction.
    // Use pipes to determine which child has successfully changed
    // session. This is needed as the setsid call can fail from other
    // processes having the same group id.
    int pipes[2];
    if (pipe(pipes) < 0) {
      perror("Failed to create a pipe");
      abort();
    }

    // Set the FD_CLOEXEC flags on these pipes.
    Try<Nothing> cloexec = os::cloexec(pipes[0]);
    if (cloexec.isError()) {
      cerr << "Failed to cloexec(pipe[0]): " << cloexec.error() << endl;
      abort();
    }

    cloexec = os::cloexec(pipes[1]);
    if (cloexec.isError()) {
      cerr << "Failed to cloexec(pipe[1]): " << cloexec.error() << endl;
      abort();
    }

    // Prepare the argv before fork as it's not async signal safe.
    char **argv = new char*[task.command().arguments().size() + 1];
    for (int i = 0; i < task.command().arguments().size(); i++) {
      argv[i] = (char*) task.command().arguments(i).c_str();
    }
    argv[task.command().arguments().size()] = NULL;

    // Prepare the command log message.
    string command;
    if (override.isSome()) {
      char** argv = override.get();
      // argv is guaranteed to be NULL terminated and we rely on
      // that fact to print command to be executed.
      for (int i = 0; argv[i] != NULL; i++) {
        command += string(argv[i]) + " ";
      }
    } else if (task.command().shell()) {
      command = "sh -c '" + task.command().value() + "'";
    } else {
      command =
        "[" + task.command().value() + ", " +
        strings::join(", ", task.command().arguments()) + "]";
    }

    if ((pid = fork()) == -1) {
      cerr << "Failed to fork to run " << command << ": "
           << strerror(errno) << endl;
      abort();
    }

    // TODO(jieyu): Make the child process async signal safe.
    if (pid == 0) {
      // In child process, we make cleanup easier by putting process
      // into it's own session.
      os::close(pipes[0]);

      // NOTE: We setsid() in a loop because setsid() might fail if another
      // process has the same process group id as the calling process.
      while ((pid = setsid()) == -1) {
        perror("Could not put command in its own session, setsid");

        cout << "Forking another process and retrying" << endl;

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

      cout << command << endl;

      // The child has successfully setsid, now run the command.
      if (override.isNone()) {
        if (task.command().shell()) {
          execl(
              "/bin/sh",
              "sh",
              "-c",
              task.command().value().c_str(),
              (char*) NULL);
        } else {
          execvp(task.command().value().c_str(), argv);
        }
      } else {
        char** argv = override.get();
        execvp(argv[0], argv);
      }

      perror("Failed to exec");
      abort();
    }

    delete[] argv;

    // In parent process.
    os::close(pipes[1]);

    // Get the child's pid via the pipe.
    if (read(pipes[0], &pid, sizeof(pid)) == -1) {
      cerr << "Failed to get child PID from pipe, read: " << strerror(errno)
           << endl;
      abort();
    }

    os::close(pipes[0]);

    cout << "Forked command at " << pid << endl;

    launchHealthCheck(task);

    // Monitor this process.
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
    if (healthPid != -1) {
      // Cleanup health check process.
      ::kill(healthPid, SIGKILL);
    }
  }

  void frameworkMessage(ExecutorDriver* driver, const string& data) {}

  void shutdown(ExecutorDriver* driver)
  {
    cout << "Shutting down" << endl;

    if (pid > 0 && !killed) {
      cout << "Sending SIGTERM to process tree at pid "
           << pid << endl;

      Try<std::list<os::ProcessTree> > trees =
        os::killtree(pid, SIGTERM, true, true);

      if (trees.isError()) {
        cerr << "Failed to kill the process tree rooted at pid "
             << pid << ": " << trees.error() << endl;

        // Send SIGTERM directly to process 'pid' as it may not have
        // received signal before os::killtree() failed.
        ::kill(pid, SIGTERM);
      } else {
        cout << "Killing the following process trees:\n"
             << stringify(trees.get()) << endl;
      }

      // TODO(nnielsen): Make escalationTimeout configurable through
      // slave flags and/or per-framework/executor.
      escalationTimer = delay(
          shutdownTimeout,
          self(),
          &Self::escalated);

      killed = true;
    }
  }

  virtual void error(ExecutorDriver* driver, const string& message) {}

protected:
  virtual void initialize()
  {
    install<TaskHealthStatus>(
        &CommandExecutorProcess::taskHealthUpdated,
        &TaskHealthStatus::task_id,
        &TaskHealthStatus::healthy,
        &TaskHealthStatus::kill_task);
  }

  void taskHealthUpdated(
      const TaskID& taskID,
      const bool& healthy,
      const bool& initiateTaskKill)
  {
    if (driver.isNone()) {
      return;
    }

    cout << "Received task health update, healthy: "
         << stringify(healthy) << endl;

    TaskStatus status;
    status.mutable_task_id()->CopyFrom(taskID);
    status.set_healthy(healthy);
    status.set_state(TASK_RUNNING);
    driver.get()->sendStatusUpdate(status);

    if (initiateTaskKill) {
      killedByHealthCheck = true;
      killTask(driver.get(), taskID);
    }
  }


private:
  void reaped(
      ExecutorDriver* driver,
      const TaskID& taskId,
      pid_t pid,
      const Future<Option<int> >& status_)
  {
    TaskState state;
    string message;

    Clock::cancel(escalationTimer);

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

      message = "Command " + WSTRINGIFY(status);
    }

    cout << message << " (pid: " << pid << ")" << endl;

    TaskStatus taskStatus;
    taskStatus.mutable_task_id()->MergeFrom(taskId);
    taskStatus.set_state(state);
    taskStatus.set_message(message);
    if (killed && killedByHealthCheck) {
      taskStatus.set_healthy(false);
    }

    driver->sendStatusUpdate(taskStatus);

    // A hack for now ... but we need to wait until the status update
    // is sent to the slave before we shut ourselves down.
    os::sleep(Seconds(1));
    driver->stop();
  }

  void escalated()
  {
    // TODO(alex): If the escalation timeout is too small, the process
    // may have already exited, but not yet reaped. If this is the
    // case, do not kill the process, since its OS pid could have been
    // already reused.
    cout << "Process " << pid << " did not terminate after "
         << shutdownTimeout << ", sending SIGKILL to "
         << "process tree at " << pid << endl;

    // TODO(nnielsen): Sending SIGTERM in the first stage of the
    // shutdown may leave orphan processes hanging off init. This
    // scenario will be handled when PID namespace encapsulated
    // execution is in place.
    Try<std::list<os::ProcessTree> > trees =
      os::killtree(pid, SIGKILL, true, true);

    if (trees.isError()) {
      cerr << "Failed to kill the process tree rooted at pid "
           << pid << ": " << trees.error() << endl;

      // Process 'pid' may not have received signal before
      // os::killtree() failed. To make sure process 'pid' is reaped
      // we send SIGKILL directly.
      ::kill(pid, SIGKILL);
    } else {
      cout << "Killed the following process trees:\n" << stringify(trees.get())
           << endl;
    }
  }

  void launchHealthCheck(const TaskInfo& task)
  {
    if (task.has_health_check()) {
      JSON::Object json = JSON::Protobuf(task.health_check());

      // Launch the subprocess using 'exec' style so that quotes can
      // be properly handled.
      vector<string> argv(4);
      argv[0] = "mesos-health-check";
      argv[1] = "--executor=" + stringify(self());
      argv[2] = "--health_check_json=" + stringify(json);
      argv[3] = "--task_id=" + task.task_id().value();

      cout << "Launching health check process: "
           << path::join(healthCheckDir, "mesos-health-check")
           << " " << argv[1] << " " << argv[2] << " " << argv[3] << endl;

      Try<Subprocess> healthProcess =
        process::subprocess(
          path::join(healthCheckDir, "mesos-health-check"),
          argv,
          // Intentionally not sending STDIN to avoid health check
          // commands that expect STDIN input to block.
          Subprocess::PATH("/dev/null"),
          Subprocess::FD(STDOUT_FILENO),
          Subprocess::FD(STDERR_FILENO));

      if (healthProcess.isError()) {
        cerr << "Unable to launch health process: " << healthProcess.error();
      } else {
        healthPid = healthProcess.get().pid();

        cout << "Health check process launched at pid: "
             << stringify(healthPid) << endl;
      }
    }
  }

  bool launched;
  bool killed;
  bool killedByHealthCheck;
  pid_t pid;
  pid_t healthPid;
  Duration shutdownTimeout;
  Timer escalationTimer;
  Option<ExecutorDriver*> driver;
  string healthCheckDir;
  Option<char**> override;
};


class CommandExecutor: public Executor
{
public:
  CommandExecutor(
      Option<char**> override,
      const string& healthCheckDir,
      const Duration& shutdownTimeout)
  {
    process = new CommandExecutorProcess(
        override,
        healthCheckDir,
        shutdownTimeout);
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
    add(&override,
        "override",
        "Whether to override the command the executor should run when the\n"
        "task is launched. Only this flag is expected to be on the command\n"
        "line and all arguments after the flag will be used as the\n"
        "subsequent 'argv' to be used with 'execvp'",
        false);

    // TODO(nnielsen): Add 'prefix' option to enable replacing
    // 'sh -c' with user specified wrapper.
  }

  bool override;
};


int main(int argc, char** argv)
{
  Flags flags;

  bool help;
  flags.add(&help,
            "help",
            "Prints this help message",
            false);

  // Get the appropriate shutdown grace period.
  Duration shutdownTimeout = EXECUTOR_SHUTDOWN_GRACE_PERIOD;
  string value = os::getenv("MESOS_SHUTDOWN_GRACE_PERIOD", false);
  if (!value.empty()) {
    Try<Duration> parse = Duration::parse(value);
    if (parse.isSome()) {
      shutdownTimeout = parse.get();
    } else {
      cerr << "Cannot parse MESOS_SHUTDOWN_GRACE_PERIOD '" << value << "': "
           << parse.error() << endl;
    }
  } else {
    cout << "Environment variable MESOS_SHUTDOWN_GRACE_PERIOD is not set, "
         << "using default value: " << shutdownTimeout << endl;
  }

  shutdownTimeout = getExecutorGracePeriod(shutdownTimeout);
  cout << "Shutdown timeout is set to " << shutdownTimeout;

  // Load flags from command line.
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

  // After flags.load(..., &argc, &argv) all flags will have been
  // stripped from argv. Additionally, arguments after a "--"
  // terminator will be preservered in argv and it is therefore
  // possible to pass override and prefix commands which use
  // "--foobar" style flags.
  Option<char**> override = None();
  if (flags.override) {
    if (argc > 1) {
      override = argv + 1;
    }
  }

  string path = os::getenv("MESOS_LAUNCHER_DIR", false);
  if (path.empty()) {
    path = os::realpath(dirname(argv[0])).get();
  }
  mesos::CommandExecutor executor(override, path, shutdownTimeout);
  mesos::MesosExecutorDriver driver(&executor);
  return driver.run() == mesos::DRIVER_STOPPED ? 0 : 1;
}
