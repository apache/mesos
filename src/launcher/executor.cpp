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
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include "common/http.hpp"
#include "common/status_utils.hpp"

#ifdef __linux__
#include "linux/fs.hpp"
#endif

#include "logging/logging.hpp"

#include "messages/messages.hpp"

#include "slave/constants.hpp"

using namespace mesos::internal::slave;

using process::wait; // Necessary on some OS's to disambiguate.

using std::cout;
using std::cerr;
using std::endl;
using std::string;
using std::vector;

namespace mesos {
namespace internal {

using namespace process;

class CommandExecutorProcess : public ProtobufProcess<CommandExecutorProcess>
{
public:
  CommandExecutorProcess(
      const Option<char**>& override,
      const string& _healthCheckDir,
      const Option<string>& _sandboxDirectory,
      const Option<string>& _workingDirectory,
      const Option<string>& _user,
      const Option<string>& _taskCommand)
    : state(REGISTERING),
      launched(false),
      killed(false),
      killedByHealthCheck(false),
      pid(-1),
      healthPid(-1),
      escalationTimeout(slave::EXECUTOR_SIGNAL_ESCALATION_TIMEOUT),
      driver(None()),
      healthCheckDir(_healthCheckDir),
      override(override),
      sandboxDirectory(_sandboxDirectory),
      workingDirectory(_workingDirectory),
      user(_user),
      taskCommand(_taskCommand) {}

  virtual ~CommandExecutorProcess() {}

  void registered(
      ExecutorDriver* _driver,
      const ExecutorInfo& _executorInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo)
  {
    CHECK_EQ(REGISTERING, state);

    cout << "Registered executor on " << slaveInfo.hostname() << endl;
    driver = _driver;
    state = REGISTERED;
  }

  void reregistered(
      ExecutorDriver* driver,
      const SlaveInfo& slaveInfo)
  {
    CHECK(state == REGISTERED || state == REGISTERING) << state;

    cout << "Re-registered executor on " << slaveInfo.hostname() << endl;
    state = REGISTERED;
  }

  void disconnected(ExecutorDriver* driver) {}

  void launchTask(ExecutorDriver* driver, const TaskInfo& task)
  {
    CHECK_EQ(REGISTERED, state);

    if (launched) {
      TaskStatus status;
      status.mutable_task_id()->MergeFrom(task.task_id());
      status.set_state(TASK_FAILED);
      status.set_message(
          "Attempted to run multiple tasks using a \"command\" executor");

      driver->sendStatusUpdate(status);
      return;
    }

    // Determine the command to launch the task.
    CommandInfo command;

    if (taskCommand.isSome()) {
      // Get CommandInfo from a JSON string.
      Try<JSON::Object> object = JSON::parse<JSON::Object>(taskCommand.get());
      if (object.isError()) {
        cerr << "Failed to parse JSON: " << object.error() << endl;
        abort();
      }

      Try<CommandInfo> parse = protobuf::parse<CommandInfo>(object.get());
      if (parse.isError()) {
        cerr << "Failed to parse protobuf: " << parse.error() << endl;
        abort();
      }

      command = parse.get();
    } else if (task.has_command()) {
      command = task.command();
    } else {
      CHECK_SOME(override)
        << "Expecting task '" << task.task_id()
        << "' to have a command!";
    }

    if (override.isNone()) {
      // TODO(jieyu): For now, we just fail the executor if the task's
      // CommandInfo is not valid. The framework will receive
      // TASK_FAILED for the task, and will most likely find out the
      // cause with some debugging. This is a temporary solution. A more
      // correct solution is to perform this validation at master side.
      if (command.shell()) {
        CHECK(command.has_value())
          << "Shell command of task '" << task.task_id()
          << "' is not specified!";
      } else {
        CHECK(command.has_value())
          << "Executable of task '" << task.task_id()
          << "' is not specified!";
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

    Option<string> rootfs;
    if (sandboxDirectory.isSome()) {
      // If 'sandbox_diretory' is specified, that means the user
      // task specifies a root filesystem, and that root filesystem has
      // already been prepared at COMMAND_EXECUTOR_ROOTFS_CONTAINER_PATH.
      // The command executor is responsible for mounting the sandbox
      // into the root filesystem, chrooting into it and changing the
      // user before exec-ing the user process.
      //
      // TODO(gilbert): Consider a better way to detect if a root
      // filesystem is specified for the command task.
#ifdef __linux__
      Result<string> user = os::user();
      if (user.isError()) {
        cerr << "Failed to get current user: " << user.error() << endl;
        abort();
      } else if (user.isNone()) {
        cerr << "Current username is not found" << endl;
        abort();
      } else if (user.get() != "root") {
        cerr << "The command executor requires root with rootfs" << endl;
        abort();
      }

      rootfs = path::join(
          os::getcwd(), COMMAND_EXECUTOR_ROOTFS_CONTAINER_PATH);

      string sandbox = path::join(rootfs.get(), sandboxDirectory.get());
      if (!os::exists(sandbox)) {
        Try<Nothing> mkdir = os::mkdir(sandbox);
        if (mkdir.isError()) {
          cerr << "Failed to create sandbox mount point  at '"
               << sandbox << "': " << mkdir.error() << endl;
          abort();
        }
      }

      // Mount the sandbox into the container rootfs.
      // NOTE: We don't use MS_REC here because the rootfs is already
      // under the sandbox.
      Try<Nothing> mount = fs::mount(
          os::getcwd(),
          sandbox,
          None(),
          MS_BIND,
          NULL);

      if (mount.isError()) {
        cerr << "Unable to mount the work directory into container "
             << "rootfs: " << mount.error() << endl;;
        abort();
      }
#else
      cerr << "Not expecting root volume with non-linux platform." << endl;
      abort();
#endif // __linux__
    }

    // Prepare the argv before fork as it's not async signal safe.
    char **argv = new char*[command.arguments().size() + 1];
    for (int i = 0; i < command.arguments().size(); i++) {
      argv[i] = (char*) command.arguments(i).c_str();
    }
    argv[command.arguments().size()] = NULL;

    // Prepare the command log message.
    string commandString;
    if (override.isSome()) {
      char** argv = override.get();
      // argv is guaranteed to be NULL terminated and we rely on
      // that fact to print command to be executed.
      for (int i = 0; argv[i] != NULL; i++) {
        commandString += string(argv[i]) + " ";
      }
    } else if (command.shell()) {
      commandString = "sh -c '" + command.value() + "'";
    } else {
      commandString =
        "[" + command.value() + ", " +
        strings::join(", ", command.arguments()) + "]";
    }

    if ((pid = fork()) == -1) {
      cerr << "Failed to fork to run " << commandString << ": "
           << os::strerror(errno) << endl;
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

      if (rootfs.isSome()) {
#ifdef __linux__
        if (user.isSome()) {
          // This is a work around to fix the problem that after we chroot
          // os::su call afterwards failed because the linker may not be
          // able to find the necessary library in the rootfs.
          // We call os::su before chroot here to force the linker to load
          // into memory.
          // We also assume it's safe to su to "root" user since
          // filesystem/linux.cpp checks for root already.
          os::su("root");
        }

        Try<Nothing> chroot = fs::chroot::enter(rootfs.get());
        if (chroot.isError()) {
          cerr << "Failed to enter chroot '" << rootfs.get()
               << "': " << chroot.error() << endl;;
          abort();
        }

        // Determine the current working directory for the executor.
        string cwd;
        if (workingDirectory.isSome()) {
          cwd = workingDirectory.get();
        } else {
          CHECK_SOME(sandboxDirectory);
          cwd = sandboxDirectory.get();
        }

        Try<Nothing> chdir = os::chdir(cwd);
        if (chdir.isError()) {
          cerr << "Failed to chdir into current working directory '"
               << cwd << "': " << chdir.error() << endl;
          abort();
        }

        if (user.isSome()) {
          Try<Nothing> su = os::su(user.get());
          if (su.isError()) {
            cerr << "Failed to change user to '" << user.get() << "': "
                 << su.error() << endl;
            abort();
          }
        }
#else
        cerr << "Rootfs is only supported on Linux" << endl;
        abort();
#endif // __linux__
      }


      cout << commandString << endl;

      // The child has successfully setsid, now run the command.
      if (override.isNone()) {
        if (command.shell()) {
          execlp(
              "sh",
              "sh",
              "-c",
              command.value().c_str(),
              (char*) NULL);
        } else {
          execvp(command.value().c_str(), argv);
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
      cerr << "Failed to get child PID from pipe, read: "
           << os::strerror(errno) << endl;
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
      os::killtree(healthPid, SIGKILL);
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
          escalationTimeout,
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
    TaskState taskState;
    string message;

    Clock::cancel(escalationTimer);

    if (!status_.isReady()) {
      taskState = TASK_FAILED;
      message =
        "Failed to get exit status for Command: " +
        (status_.isFailed() ? status_.failure() : "future discarded");
    } else if (status_.get().isNone()) {
      taskState = TASK_FAILED;
      message = "Failed to get exit status for Command";
    } else {
      int status = status_.get().get();
      CHECK(WIFEXITED(status) || WIFSIGNALED(status)) << status;

      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        taskState = TASK_FINISHED;
      } else if (killed) {
        // Send TASK_KILLED if the task was killed as a result of
        // killTask() or shutdown().
        taskState = TASK_KILLED;
      } else {
        taskState = TASK_FAILED;
      }

      message = "Command " + WSTRINGIFY(status);
    }

    cout << message << " (pid: " << pid << ")" << endl;

    TaskStatus taskStatus;
    taskStatus.mutable_task_id()->MergeFrom(taskId);
    taskStatus.set_state(taskState);
    taskStatus.set_message(message);
    if (killed && killedByHealthCheck) {
      taskStatus.set_healthy(false);
    }

    driver->sendStatusUpdate(taskStatus);

    // This is a hack to ensure the message is sent to the
    // slave before we exit the process. Without this, we
    // may exit before libprocess has sent the data over
    // the socket. See MESOS-4111.
    os::sleep(Seconds(1));
    driver->stop();
  }

  void escalated()
  {
    cout << "Process " << pid << " did not terminate after "
         << escalationTimeout << ", sending SIGKILL to "
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
      JSON::Object json = JSON::protobuf(task.health_check());

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

  enum State
  {
    REGISTERING, // Executor is launched but not (re-)registered yet.
    REGISTERED,  // Executor has (re-)registered.
  } state;

  bool launched;
  bool killed;
  bool killedByHealthCheck;
  pid_t pid;
  pid_t healthPid;
  Duration escalationTimeout;
  Timer escalationTimer;
  Option<ExecutorDriver*> driver;
  string healthCheckDir;
  Option<char**> override;
  Option<string> sandboxDirectory;
  Option<string> workingDirectory;
  Option<string> user;
  Option<string> taskCommand;
};


class CommandExecutor: public Executor
{
public:
  CommandExecutor(
      const Option<char**>& override,
      const string& healthCheckDir,
      const Option<string>& sandboxDirectory,
      const Option<string>& workingDirectory,
      const Option<string>& user,
      const Option<string>& taskCommand)
  {
    process = new CommandExecutorProcess(override,
                                         healthCheckDir,
                                         sandboxDirectory,
                                         workingDirectory,
                                         user,
                                         taskCommand);

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


class Flags : public flags::FlagsBase
{
public:
  Flags()
  {
    // TODO(gilbert): Deprecate the 'override' flag since no one is
    // using it, and it may cause confusing with 'task_command' flag.
    add(&override,
        "override",
        "Whether to override the command the executor should run when the\n"
        "task is launched. Only this flag is expected to be on the command\n"
        "line and all arguments after the flag will be used as the\n"
        "subsequent 'argv' to be used with 'execvp'",
        false);

    // The following flags are only applicable when a rootfs is
    // provisioned for this command.
    add(&sandbox_directory,
        "sandbox_directory",
        "The absolute path for the directory in the container where the\n"
        "sandbox is mapped to");

    add(&working_directory,
        "working_directory",
        "The working directory for the task in the container.");

    add(&user,
        "user",
        "The user that the task should be running as.");

    add(&task_command,
        "task_command",
        "If specified, this is the overrided command for launching the\n"
        "task (instead of the command from TaskInfo).");

    // TODO(nnielsen): Add 'prefix' option to enable replacing
    // 'sh -c' with user specified wrapper.
  }

  bool override;
  Option<string> sandbox_directory;
  Option<string> working_directory;
  Option<string> user;
  Option<string> task_command;
};


int main(int argc, char** argv)
{
  Flags flags;

  // Load flags from command line.
  Try<Nothing> load = flags.load(None(), &argc, &argv);

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
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

  const Option<string> envPath = os::getenv("MESOS_LAUNCHER_DIR");

  string path = envPath.isSome()
    ? envPath.get()
    : os::realpath(Path(argv[0]).dirname()).get();

  mesos::internal::CommandExecutor executor(
      override,
      path,
      flags.sandbox_directory,
      flags.working_directory,
      flags.user,
      flags.task_command);

  mesos::MesosExecutorDriver driver(&executor);

  return driver.run() == mesos::DRIVER_STOPPED ? EXIT_SUCCESS : EXIT_FAILURE;
}
