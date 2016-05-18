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

#include <mesos/v1/executor.hpp>
#include <mesos/v1/mesos.hpp>

#include <mesos/type_utils.hpp>

#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/subprocess.hpp>
#include <process/reap.hpp>
#include <process/time.hpp>
#include <process/timer.hpp>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include "common/http.hpp"
#include "common/status_utils.hpp"

#include "internal/evolve.hpp"

#ifdef __linux__
#include "linux/fs.hpp"
#endif

#include "executor/v0_v1executor.hpp"

#include "logging/logging.hpp"

#include "messages/messages.hpp"

#include "slave/constants.hpp"

#ifdef __linux__
namespace fs = mesos::internal::fs;
#endif

using namespace mesos::internal::slave;

using std::cout;
using std::cerr;
using std::endl;
using std::queue;
using std::string;
using std::vector;

using process::Clock;
using process::Future;
using process::Owned;
using process::Subprocess;
using process::Time;
using process::Timer;

using mesos::internal::evolve;
using mesos::internal::TaskHealthStatus;

using mesos::v1::ExecutorID;
using mesos::v1::FrameworkID;

using mesos::v1::executor::Call;
using mesos::v1::executor::Event;
using mesos::v1::executor::Mesos;
using mesos::v1::executor::MesosBase;
using mesos::v1::executor::V0ToV1Adapter;

namespace mesos {
namespace v1 {
namespace internal {

class CommandExecutor: public ProtobufProcess<CommandExecutor>
{
public:
  CommandExecutor(
      const Option<char**>& _override,
      const string& _healthCheckDir,
      const Option<string>& _rootfs,
      const Option<string>& _sandboxDirectory,
      const Option<string>& _workingDirectory,
      const Option<string>& _user,
      const Option<string>& _taskCommand,
      const FrameworkID& _frameworkId,
      const ExecutorID& _executorId,
      const Duration& _shutdownGracePeriod)
    : state(DISCONNECTED),
      launched(false),
      killed(false),
      killedByHealthCheck(false),
      terminated(false),
      pid(-1),
      healthPid(-1),
      shutdownGracePeriod(_shutdownGracePeriod),
      frameworkInfo(None()),
      taskId(None()),
      healthCheckDir(_healthCheckDir),
      override(_override),
      rootfs(_rootfs),
      sandboxDirectory(_sandboxDirectory),
      workingDirectory(_workingDirectory),
      user(_user),
      taskCommand(_taskCommand),
      frameworkId(_frameworkId),
      executorId(_executorId),
      task(None()) {}

  virtual ~CommandExecutor() = default;

  void connected()
  {
    state = CONNECTED;

    doReliableRegistration();
  }

  void disconnected()
  {
    state = DISCONNECTED;
  }

  void received(queue<Event> events)
  {
    while (!events.empty()) {
      Event event = events.front();
      events.pop();

      cout << "Received " << event.type() << " event" << endl;

      switch (event.type()) {
        case Event::SUBSCRIBED: {
          cout << "Subscribed executor on "
               << event.subscribed().agent_info().hostname() << endl;

          frameworkInfo = event.subscribed().framework_info();
          state = SUBSCRIBED;
          break;
        }

        case Event::LAUNCH: {
          launch(event.launch().task());
          break;
        }

        case Event::KILL: {
          Option<KillPolicy> override = event.kill().has_kill_policy()
            ? Option<KillPolicy>(event.kill().kill_policy())
            : None();

          kill(event.kill().task_id(), override);
          break;
        }

        case Event::ACKNOWLEDGED: {
          // Remove the corresponding update.
          updates.erase(UUID::fromBytes(event.acknowledged().uuid()));

          // Remove the corresponding task.
          task = None();
          break;
        }

        case Event::SHUTDOWN: {
          shutdown();
          break;
        }

        case Event::MESSAGE: {
          break;
        }

        case Event::ERROR: {
          cerr << "Error: " << event.error().message() << endl;
          break;
        }

        case Event::UNKNOWN: {
          LOG(WARNING) << "Received an UNKNOWN event and ignored";
          break;
        }
      }
    }
  }

protected:
  virtual void initialize()
  {
    // TODO(qianzhang): Currently, the `mesos-health-check` binary can only
    // send unversioned `TaskHealthStatus` messages. This needs to be revisited
    // as part of MESOS-5103.
    install<TaskHealthStatus>(
        &CommandExecutor::taskHealthUpdated,
        &TaskHealthStatus::task_id,
        &TaskHealthStatus::healthy,
        &TaskHealthStatus::kill_task);

    Option<string> value = os::getenv("MESOS_HTTP_COMMAND_EXECUTOR");

    // We initialize the library here to ensure that callbacks are only invoked
    // after the process has spawned.
    if (value.isSome() && value.get() == "1") {
      mesos.reset(new Mesos(
          mesos::ContentType::PROTOBUF,
          defer(self(), &Self::connected),
          defer(self(), &Self::disconnected),
          defer(self(), &Self::received, lambda::_1)));
    } else {
      mesos.reset(new V0ToV1Adapter(
          defer(self(), &Self::connected),
          defer(self(), &Self::disconnected),
          defer(self(), &Self::received, lambda::_1)));
    }
  }

  void taskHealthUpdated(
      const mesos::TaskID& taskID,
      const bool healthy,
      const bool initiateTaskKill)
  {
    cout << "Received task health update, healthy: "
         << stringify(healthy) << endl;

    update(evolve(taskID), TASK_RUNNING, healthy);

    if (initiateTaskKill) {
      killedByHealthCheck = true;
      kill(evolve(taskID));
    }
  }

  void doReliableRegistration()
  {
    if (state == SUBSCRIBED || state == DISCONNECTED) {
      return;
    }

    Call call;
    call.set_type(Call::SUBSCRIBE);

    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(executorId);

    Call::Subscribe* subscribe = call.mutable_subscribe();

    // Send all unacknowledged updates.
    foreach (const Call::Update& update, updates.values()) {
      subscribe->add_unacknowledged_updates()->MergeFrom(update);
    }

    // Send the unacknowledged task.
    if (task.isSome()) {
      subscribe->add_unacknowledged_tasks()->MergeFrom(task.get());
    }

    mesos->send(call);

    delay(Seconds(1), self(), &Self::doReliableRegistration);
  }

  void launch(const TaskInfo& _task)
  {
    CHECK_EQ(SUBSCRIBED, state);

    if (launched) {
      update(
          _task.task_id(),
          TASK_FAILED,
          None(),
          "Attempted to run multiple tasks using a \"command\" executor");
      return;
    }

    // Capture the task.
    task = _task;

    // Capture the TaskID.
    taskId = task->task_id();

    // Capture the kill policy.
    if (task->has_kill_policy()) {
      killPolicy = task->kill_policy();
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
    } else if (task->has_command()) {
      command = task->command();
    } else {
      CHECK_SOME(override)
        << "Expecting task '" << task->task_id()
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
          << "Shell command of task '" << task->task_id()
          << "' is not specified!";
      } else {
        CHECK(command.has_value())
          << "Executable of task '" << task->task_id()
          << "' is not specified!";
      }
    }

    cout << "Starting task " << task->task_id() << endl;

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

    if (rootfs.isSome()) {
      // The command executor is responsible for chrooting into the
      // root filesystem and changing the user before exec-ing the
      // user process.
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

    if (task->has_health_check()) {
      launchHealthCheck(task.get());
    }

    // Monitor this process.
    process::reap(pid)
      .onAny(defer(self(), &Self::reaped, pid, lambda::_1));

    update(task->task_id(), TASK_RUNNING);

    launched = true;
  }

  void kill(const TaskID& taskId, const Option<KillPolicy>& override = None())
  {
    // Default grace period is set to 3s for backwards compatibility.
    //
    // TODO(alexr): Replace it with a more meaningful default, e.g.
    // `shutdownGracePeriod` after the deprecation cycle, started in 0.29.
    Duration gracePeriod = Seconds(3);

    // Kill policy provided in the `Kill` event takes precedence
    // over kill policy specified when the task was launched.
    if (override.isSome() && override->has_grace_period()) {
      gracePeriod = Nanoseconds(override->grace_period().nanoseconds());
    } else if (killPolicy.isSome() && killPolicy->has_grace_period()) {
      gracePeriod = Nanoseconds(killPolicy->grace_period().nanoseconds());
    }

    cout << "Received kill for task " << taskId.value()
         << " with grace period of " << gracePeriod << endl;

    kill(taskId, gracePeriod);
  }

  void shutdown()
  {
    cout << "Shutting down" << endl;

    // NOTE: We leave a small buffer of time to do the forced kill, otherwise
    // the agent may destroy the container before we can send `TASK_KILLED`.
    //
    // TODO(alexr): Remove `MAX_REAP_INTERVAL` once the reaper signals
    // immediately after the watched process has exited.
    Duration gracePeriod =
      shutdownGracePeriod - process::MAX_REAP_INTERVAL() - Seconds(1);

    // Since the command executor manages a single task,
    // shutdown boils down to killing this task.
    //
    // TODO(bmahler): If a shutdown arrives after a kill task within
    // the grace period of the `KillPolicy`, we may need to escalate
    // more quickly (e.g. the shutdown grace period allotted by the
    // agent is smaller than the kill grace period).
    if (launched) {
      CHECK_SOME(taskId);
      kill(taskId.get(), gracePeriod);
    }
  }

private:
  void kill(const TaskID& _taskId, const Duration& gracePeriod)
  {
    if (terminated) {
      return;
    }

    // If the task is being killed but has not terminated yet and
    // we receive another kill request. Check if we need to adjust
    // the remaining grace period.
    if (killed && !terminated) {
      // When a kill request arrives on the executor, we cannot simply
      // restart the escalation timer, because the scheduler may retry
      // and this must be a no-op.
      //
      // The escalation grace period can be only decreased. We disallow
      // increasing the total grace period for the terminating task in
      // order to avoid possible confusion when a subsequent kill overrides
      // the previous one and gives the task _more_ time to clean up. Other
      // systems, e.g., docker, do not allow this.
      //
      // The escalation grace period can be only decreased. We intentionally
      // do not support increasing the total grace period for the terminating
      // task, because we do not want users to "slow down" a kill that is in
      // progress. Also note that docker does not support this currently.
      //
      // Here are some examples to illustrate:
      //
      // 20, 30 -> Increased grace period is a no-op, grace period remains 20.
      // 20, 20 -> Retries are a no-op, grace period remains 20.
      // 20, 5  -> if `elapsed` >= 5:
      //             SIGKILL immediately, total grace period is `elapsed`.
      //           if `elapsed` < 5:
      //             SIGKILL in (5 - `elapsed`), total grace period is 5.

      CHECK_SOME(killGracePeriodStart);
      CHECK_SOME(killGracePeriodTimer);

      if (killGracePeriodStart.get() + gracePeriod >
          killGracePeriodTimer->timeout().time()) {
        return;
      }

      Duration elapsed = Clock::now() - killGracePeriodStart.get();
      Duration remaining = gracePeriod > elapsed
        ? gracePeriod - elapsed
        : Duration::zero();

      cout << "Rescheduling escalation to SIGKILL in " << remaining
           << " from now" << endl;

      Clock::cancel(killGracePeriodTimer.get());
      killGracePeriodTimer = delay(
          remaining, self(), &Self::escalated, gracePeriod);
    }

    // Issue the kill signal if the task has been launched
    // and this is the first time we've received the kill.
    if (launched && !killed) {
      // Send TASK_KILLING if the framework can handle it.
      CHECK_SOME(frameworkInfo);
      CHECK_SOME(taskId);
      CHECK(taskId.get() == _taskId);

      foreach (const FrameworkInfo::Capability& c,
               frameworkInfo->capabilities()) {
        if (c.type() == FrameworkInfo::Capability::TASK_KILLING_STATE) {
          update(taskId.get(), TASK_KILLING);
          break;
        }
      }

      // Now perform signal escalation to begin killing the task.
      CHECK_GT(pid, 0);

      cout << "Sending SIGTERM to process tree at pid " << pid << endl;

      Try<std::list<os::ProcessTree> > trees =
        os::killtree(pid, SIGTERM, true, true);

      if (trees.isError()) {
        cerr << "Failed to kill the process tree rooted at pid " << pid
             << ": " << trees.error() << endl;

        // Send SIGTERM directly to process 'pid' as it may not have
        // received signal before os::killtree() failed.
        ::kill(pid, SIGTERM);
      } else {
        cout << "Sent SIGTERM to the following process trees:\n"
             << stringify(trees.get()) << endl;
      }

      cout << "Scheduling escalation to SIGKILL in " << gracePeriod
           << " from now" << endl;

      killGracePeriodTimer =
        delay(gracePeriod, self(), &Self::escalated, gracePeriod);

      killGracePeriodStart = Clock::now();
      killed = true;
    }

    // Cleanup health check process.
    //
    // TODO(bmahler): Consider doing this after the task has been
    // reaped, since a framework may be interested in health
    // information while the task is being killed (consider a
    // task that takes 30 minutes to be cleanly killed).
    if (healthPid != -1) {
      os::killtree(healthPid, SIGKILL);
    }
  }

  void reaped(pid_t pid, const Future<Option<int> >& status_)
  {
    terminated = true;

    TaskState taskState;
    string message;

    if (killGracePeriodTimer.isSome()) {
      Clock::cancel(killGracePeriodTimer.get());
    }

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
        // kill() or shutdown().
        taskState = TASK_KILLED;
      } else {
        taskState = TASK_FAILED;
      }

      message = "Command " + WSTRINGIFY(status);
    }

    cout << message << " (pid: " << pid << ")" << endl;

    CHECK_SOME(taskId);

    if (killed && killedByHealthCheck) {
      update(taskId.get(), taskState, false, message);
    } else {
      update(taskId.get(), taskState, None(), message);
    }

    // TODO(qianzhang): Remove this hack since the executor now receives
    // acknowledgements for status updates. The executor can terminate
    // after it receives an ACK for a terminal status update.
    os::sleep(Seconds(1));
    terminate(self());
  }

  void escalated(const Duration& timeout)
  {
    if (terminated) {
      return;
    }

    cout << "Process " << pid << " did not terminate after " << timeout
         << ", sending SIGKILL to process tree at " << pid << endl;

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
    CHECK(task.has_health_check());

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
      return;
    }

    healthPid = healthProcess.get().pid();

    cout << "Health check process launched at pid: "
         << stringify(healthPid) << endl;
  }

  void update(
      const TaskID& taskID,
      const TaskState& state,
      const Option<bool>& healthy = None(),
      const Option<string>& message = None())
  {
    UUID uuid = UUID::random();

    TaskStatus status;
    status.mutable_task_id()->CopyFrom(taskID);
    status.mutable_executor_id()->CopyFrom(executorId);

    status.set_state(state);
    status.set_source(TaskStatus::SOURCE_EXECUTOR);
    status.set_uuid(uuid.toBytes());

    if (healthy.isSome()) {
      status.set_healthy(healthy.get());
    }

    if (message.isSome()) {
      status.set_message(message.get());
    }

    Call call;
    call.set_type(Call::UPDATE);

    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(executorId);

    call.mutable_update()->mutable_status()->CopyFrom(status);

    // Capture the status update.
    updates[uuid] = call.update();

    mesos->send(call);
  }

  enum State
  {
    CONNECTED,
    DISCONNECTED,
    SUBSCRIBED
  } state;

  // TODO(alexr): Introduce a state enum and document transitions,
  // see MESOS-5252.
  bool launched;
  bool killed;
  bool killedByHealthCheck;
  bool terminated;

  Option<Time> killGracePeriodStart;
  Option<Timer> killGracePeriodTimer;

  pid_t pid;
  pid_t healthPid;
  Duration shutdownGracePeriod;
  Option<KillPolicy> killPolicy;
  Option<FrameworkInfo> frameworkInfo;
  Option<TaskID> taskId;
  string healthCheckDir;
  Option<char**> override;
  Option<string> rootfs;
  Option<string> sandboxDirectory;
  Option<string> workingDirectory;
  Option<string> user;
  Option<string> taskCommand;
  const FrameworkID frameworkId;
  const ExecutorID executorId;
  Owned<MesosBase> mesos;
  LinkedHashMap<UUID, Call::Update> updates; // Unacknowledged updates.
  Option<TaskInfo> task; // Unacknowledged task.
};

} // namespace internal {
} // namespace v1 {
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

    add(&rootfs,
        "rootfs",
        "The path to the root filesystem for the task");

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
  Option<string> rootfs;
  Option<string> sandbox_directory;
  Option<string> working_directory;
  Option<string> user;
  Option<string> task_command;
};


int main(int argc, char** argv)
{
  Flags flags;
  FrameworkID frameworkId;
  ExecutorID executorId;

  // Load flags from command line.
  Try<flags::Warnings> load = flags.load(None(), &argc, &argv);

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
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

  Option<string> value = os::getenv("MESOS_FRAMEWORK_ID");
  if (value.isNone()) {
    EXIT(EXIT_FAILURE)
      << "Expecting 'MESOS_FRAMEWORK_ID' to be set in the environment";
  }
  frameworkId.set_value(value.get());

  value = os::getenv("MESOS_EXECUTOR_ID");
  if (value.isNone()) {
    EXIT(EXIT_FAILURE)
      << "Expecting 'MESOS_EXECUTOR_ID' to be set in the environment";
  }
  executorId.set_value(value.get());

  // Get executor shutdown grace period from the environment.
  //
  // NOTE: We avoided introducing a command executor flag for this
  // because the command executor exits if it sees an unknown flag.
  // This makes it difficult to add or remove command executor flags
  // that are unconditionally set by the agent.
  Duration shutdownGracePeriod = DEFAULT_EXECUTOR_SHUTDOWN_GRACE_PERIOD;
  value = os::getenv("MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD");
  if (value.isSome()) {
    Try<Duration> parse = Duration::parse(value.get());
    if (parse.isError()) {
      cerr << "Failed to parse value '" << value.get() << "'"
           << " of 'MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD': " << parse.error();
      return EXIT_FAILURE;
    }

    shutdownGracePeriod = parse.get();
  }

  Owned<mesos::v1::internal::CommandExecutor> executor(
      new mesos::v1::internal::CommandExecutor(
          override,
          path,
          flags.rootfs,
          flags.sandbox_directory,
          flags.working_directory,
          flags.user,
          flags.task_command,
          frameworkId,
          executorId,
          shutdownGracePeriod));

  process::spawn(executor.get());
  process::wait(executor.get());

  return EXIT_SUCCESS;
}
