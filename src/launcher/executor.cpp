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

#include "launcher/executor.hpp"

#include <signal.h>
#include <stdio.h>

#ifndef __WINDOWS__
#include <sys/wait.h>
#endif // __WINDOWS__

#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>
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
#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <stout/os/environment.hpp>
#include <stout/os/kill.hpp>
#include <stout/os/killtree.hpp>

#include "checks/checker.hpp"
#include "checks/health_checker.hpp"

#include "common/http.hpp"
#include "common/parse.hpp"
#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"

#include "executor/v0_v1executor.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "logging/logging.hpp"

#include "messages/messages.hpp"

#include "slave/constants.hpp"

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

using mesos::Environment;

using mesos::executor::Call;
using mesos::executor::Event;

using mesos::v1::executor::Mesos;
using mesos::v1::executor::MesosBase;
using mesos::v1::executor::V0ToV1Adapter;

namespace mesos {
namespace internal {

class CommandExecutor: public ProtobufProcess<CommandExecutor>
{
public:
  CommandExecutor(
      const string& _launcherDir,
      const Option<string>& _rootfs,
      const Option<string>& _sandboxDirectory,
      const Option<string>& _workingDirectory,
      const Option<string>& _user,
      const Option<string>& _taskCommand,
      const Option<Environment>& _taskEnvironment,
      const Option<CapabilityInfo>& _capabilities,
      const FrameworkID& _frameworkId,
      const ExecutorID& _executorId,
      const Duration& _shutdownGracePeriod)
    : ProcessBase(process::ID::generate("command-executor")),
      state(DISCONNECTED),
      taskData(None()),
      launched(false),
      killed(false),
      killedByHealthCheck(false),
      terminated(false),
      pid(-1),
      shutdownGracePeriod(_shutdownGracePeriod),
      frameworkInfo(None()),
      taskId(None()),
      launcherDir(_launcherDir),
      rootfs(_rootfs),
      sandboxDirectory(_sandboxDirectory),
      workingDirectory(_workingDirectory),
      user(_user),
      taskCommand(_taskCommand),
      taskEnvironment(_taskEnvironment),
      capabilities(_capabilities),
      frameworkId(_frameworkId),
      executorId(_executorId),
      lastTaskStatus(None())
  {
#ifdef __WINDOWS__
    processHandle = INVALID_HANDLE_VALUE;
#endif
  }

  virtual ~CommandExecutor()
  {
#ifdef __WINDOWS__
    if (processHandle != INVALID_HANDLE_VALUE) {
      ::CloseHandle(processHandle);
    }
#endif // __WINDOWS__
  }

  void connected()
  {
    state = CONNECTED;

    doReliableRegistration();
  }

  void disconnected()
  {
    state = DISCONNECTED;
  }

  void received(const Event& event)
  {
    cout << "Received " << event.type() << " event" << endl;

    switch (event.type()) {
      case Event::SUBSCRIBED: {
        cout << "Subscribed executor on "
             << event.subscribed().slave_info().hostname() << endl;

        frameworkInfo = event.subscribed().framework_info();
        state = SUBSCRIBED;
        break;
      }

      case Event::LAUNCH: {
        launch(event.launch().task());
        break;
      }

      case Event::LAUNCH_GROUP: {
        cerr << "LAUNCH_GROUP event is not supported" << endl;
        // Shut down because this is unexpected; `LAUNCH_GROUP` event
        // should only ever go to a group-capable default executor and
        // not the command executor.
        shutdown();
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
        const UUID uuid = UUID::fromBytes(event.acknowledged().uuid()).get();

        if (!unacknowledgedUpdates.contains(uuid)) {
          LOG(WARNING) << "Received acknowledgement " << uuid
                       << " for unknown status update";
          return;
        }

        // Terminate if we receive the ACK for the terminal status update.
        // NOTE: The executor receives an ACK iff it uses the HTTP library.
        // No ACK will be received if V0ToV1Adapter is used.
        if (mesos::internal::protobuf::isTerminalState(
            unacknowledgedUpdates[uuid].status().state())) {
          terminate(self());
        }

        // Remove the corresponding update.
        unacknowledgedUpdates.erase(uuid);

        // Mark task as acknowledged.
        CHECK(taskData.isSome());
        taskData->acknowledged = true;

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

protected:
  virtual void initialize()
  {
    Option<string> value = os::getenv("MESOS_HTTP_COMMAND_EXECUTOR");

    // We initialize the library here to ensure that callbacks are only invoked
    // after the process has spawned.
    if (value.isSome() && value.get() == "1") {
      mesos.reset(new Mesos(
          ContentType::PROTOBUF,
          defer(self(), &Self::connected),
          defer(self(), &Self::disconnected),
          defer(self(), [this](queue<v1::executor::Event> events) {
            while(!events.empty()) {
              const v1::executor::Event& event = events.front();
              received(devolve(event));

              events.pop();
            }
          })));
    } else {
      mesos.reset(new V0ToV1Adapter(
          defer(self(), &Self::connected),
          defer(self(), &Self::disconnected),
          defer(self(), [this](queue<v1::executor::Event> events) {
            while(!events.empty()) {
              const v1::executor::Event& event = events.front();
              received(devolve(event));

              events.pop();
            }
          })));
    }
  }

  void taskCheckUpdated(
      const TaskID& _taskId,
      const CheckStatusInfo& checkStatus)
  {
    CHECK_SOME(taskId);
    CHECK_EQ(taskId.get(), _taskId);

    // This prevents us from sending check updates after a terminal
    // status update, because we may receive an update from a check
    // scheduled before the task has been reaped.
    //
    // TODO(alexr): Consider sending check updates after TASK_KILLING.
    if (killed || terminated) {
      return;
    }

    cout << "Received check update" << endl;

    // Use the previous task status to preserve all attached information.
    CHECK_SOME(lastTaskStatus);
    TaskStatus status = protobuf::createTaskStatus(
        lastTaskStatus.get(),
        UUID::random(),
        Clock::now().secs(),
        None(),
        None(),
        None(),
        TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
        None(),
        None(),
        checkStatus);

    forward(status);
  }

  void taskHealthUpdated(const TaskHealthStatus& healthStatus)
  {
    CHECK_SOME(taskId);
    CHECK_EQ(taskId.get(), healthStatus.task_id());

    // This prevents us from sending health updates after a terminal
    // status update, because we may receive an update from a health
    // check scheduled before the task has been reaped.
    //
    // TODO(alexr): Consider sending health updates after TASK_KILLING.
    if (killed || terminated) {
      return;
    }

    cout << "Received task health update, healthy: "
         << stringify(healthStatus.healthy()) << endl;

    // Use the previous task status to preserve all attached information.
    CHECK_SOME(lastTaskStatus);
    TaskStatus status = protobuf::createTaskStatus(
        lastTaskStatus.get(),
        UUID::random(),
        Clock::now().secs(),
        None(),
        None(),
        None(),
        None(),
        None(),
        healthStatus.healthy());

    forward(status);

    if (healthStatus.kill_task()) {
      killedByHealthCheck = true;
      kill(healthStatus.task_id());
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
    foreachvalue (const Call::Update& update, unacknowledgedUpdates) {
      subscribe->add_unacknowledged_updates()->MergeFrom(update);
    }

    // Send the unacknowledged task.
    if (taskData.isSome() && !taskData->acknowledged) {
      subscribe->add_unacknowledged_tasks()->MergeFrom(
          taskData->taskInfo);
    }

    mesos->send(evolve(call));

    delay(Seconds(1), self(), &Self::doReliableRegistration);
  }

  void launch(const TaskInfo& task)
  {
    CHECK_EQ(SUBSCRIBED, state);

    if (launched) {
      TaskStatus status = createTaskStatus(
          task.task_id(),
          TASK_FAILED,
          None(),
          "Attempted to run multiple tasks using a \"command\" executor");

      forward(status);
      return;
    }

    // Capture `TaskInfo` and `TaskID` of the task.
    CHECK(taskData.isNone());
    taskData = TaskData(task);
    taskId = task.task_id();

    // Capture the kill policy.
    if (task.has_kill_policy()) {
      killPolicy = task.kill_policy();
    }

    // Determine the command to launch the task.
    CommandInfo command;

    if (taskCommand.isSome()) {
      // Get CommandInfo from a JSON string.
      Try<JSON::Object> object = JSON::parse<JSON::Object>(taskCommand.get());
      if (object.isError()) {
        ABORT("Failed to parse JSON: " + object.error());
      }

      Try<CommandInfo> parse = ::protobuf::parse<CommandInfo>(object.get());
      if (parse.isError()) {
        ABORT("Failed to parse protobuf: " + parse.error());
      }

      command = parse.get();
    } else if (task.has_command()) {
      command = task.command();
    } else {
      LOG(FATAL) << "Expecting task '" << taskId.get() << "' to have a command";
    }

    // TODO(jieyu): For now, we just fail the executor if the task's
    // CommandInfo is not valid. The framework will receive
    // TASK_FAILED for the task, and will most likely find out the
    // cause with some debugging. This is a temporary solution. A more
    // correct solution is to perform this validation at master side.
    if (command.shell()) {
      CHECK(command.has_value())
        << "Shell command of task '" << taskId.get() << "' is not specified";
    } else {
      CHECK(command.has_value())
        << "Executable of task '" << taskId.get() << "' is not specified";
    }

    // Determine the environment for the command to be launched.
    // The priority of the environment should be:
    //  1) User specified environment in CommandInfo.
    //  2) Environment returned by isolators (`taskEnvironment`).
    //  3) Environment passed from agent (`os::environment`).
    //
    // TODO(josephw): Windows tasks will inherit the environment
    // from the executor for now. Change this if a Windows isolator
    // ever uses the `--task_environment` flag.
    Environment launchEnvironment;

    foreachpair (const string& name, const string& value, os::environment()) {
      Environment::Variable* variable = launchEnvironment.add_variables();
      variable->set_name(name);
      variable->set_value(value);
    }

    if (taskEnvironment.isSome()) {
      launchEnvironment.MergeFrom(taskEnvironment.get());
    }

    if (command.has_environment()) {
      launchEnvironment.MergeFrom(command.environment());
    }

    cout << "Starting task " << taskId.get() << endl;

#ifndef __WINDOWS__
    pid = launchTaskPosix(
        command,
        launcherDir,
        launchEnvironment,
        user,
        rootfs,
        sandboxDirectory,
        workingDirectory,
        capabilities);
#else
    // A Windows process is started using the `CREATE_SUSPENDED` flag
    // and is part of a job object. While the process handle is kept
    // open the reap function will work.
    PROCESS_INFORMATION processInformation = launchTaskWindows(
        command,
        rootfs);

    pid = processInformation.dwProcessId;
    ::ResumeThread(processInformation.hThread);
    CloseHandle(processInformation.hThread);
    processHandle = processInformation.hProcess;
#endif

    cout << "Forked command at " << pid << endl;

    if (task.has_check()) {
      vector<string> namespaces;
      if (rootfs.isSome() &&
          task.check().type() == CheckInfo::COMMAND) {
        // Make sure command checks are run from the task's mount namespace.
        // Otherwise if rootfs is specified the command binary may not be
        // available in the executor.
        //
        // NOTE: The command executor shares the network namespace
        // with its task, hence no need to enter it explicitly.
        namespaces.push_back("mnt");
      }

      Try<Owned<checks::Checker>> _checker =
        checks::Checker::create(
            task.check(),
            defer(self(), &Self::taskCheckUpdated, taskId.get(), lambda::_1),
            taskId.get(),
            pid,
            namespaces);

      if (_checker.isError()) {
        // TODO(alexr): Consider ABORT and return a TASK_FAILED here.
        cerr << "Failed to create checker: " << _checker.error() << endl;
      } else {
        checker = _checker.get();
      }
    }

    if (task.has_health_check()) {
      vector<string> namespaces;
      if (rootfs.isSome() &&
          task.health_check().type() == HealthCheck::COMMAND) {
        // Make sure command health checks are run from the task's mount
        // namespace. Otherwise if rootfs is specified the command binary
        // may not be available in the executor.
        //
        // NOTE: The command executor shares the network namespace
        // with its task, hence no need to enter it explicitly.
        namespaces.push_back("mnt");
      }

      Try<Owned<checks::HealthChecker>> _healthChecker =
        checks::HealthChecker::create(
            task.health_check(),
            launcherDir,
            defer(self(), &Self::taskHealthUpdated, lambda::_1),
            taskId.get(),
            pid,
            namespaces);

      if (_healthChecker.isError()) {
        // TODO(gilbert): Consider ABORT and return a TASK_FAILED here.
        cerr << "Failed to create health checker: "
             << _healthChecker.error() << endl;
      } else {
        healthChecker = _healthChecker.get();
      }
    }

    // Monitor this process.
    process::reap(pid)
      .onAny(defer(self(), &Self::reaped, pid, lambda::_1));

    TaskStatus status = createTaskStatus(taskId.get(), TASK_RUNNING);

    forward(status);
    launched = true;
  }

  void kill(const TaskID& _taskId, const Option<KillPolicy>& override = None())
  {
    // Default grace period is set to 3s for backwards compatibility.
    //
    // TODO(alexr): Replace it with a more meaningful default, e.g.
    // `shutdownGracePeriod` after the deprecation cycle, started in 1.0.
    Duration gracePeriod = Seconds(3);

    // Kill policy provided in the `Kill` event takes precedence
    // over kill policy specified when the task was launched.
    if (override.isSome() && override->has_grace_period()) {
      gracePeriod = Nanoseconds(override->grace_period().nanoseconds());
    } else if (killPolicy.isSome() && killPolicy->has_grace_period()) {
      gracePeriod = Nanoseconds(killPolicy->grace_period().nanoseconds());
    }

    cout << "Received kill for task " << _taskId.value()
         << " with grace period of " << gracePeriod << endl;

    kill(_taskId, gracePeriod);
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

      if (protobuf::frameworkHasCapability(
              frameworkInfo.get(),
              FrameworkInfo::Capability::TASK_KILLING_STATE)) {
        TaskStatus status = createTaskStatus(taskId.get(), TASK_KILLING);
        forward(status);
      }

      // Stop checking the task.
      if (checker.get() != nullptr) {
        checker->stop();
      }

      // Stop health checking the task.
      if (healthChecker.get() != nullptr) {
        healthChecker->stop();
      }

      // Now perform signal escalation to begin killing the task.
      CHECK_GT(pid, 0);

      cout << "Sending SIGTERM to process tree at pid " << pid << endl;

      Try<std::list<os::ProcessTree>> trees =
        os::killtree(pid, SIGTERM, true, true);

      if (trees.isError()) {
        cerr << "Failed to kill the process tree rooted at pid " << pid
             << ": " << trees.error() << endl;

        // Send SIGTERM directly to process 'pid' as it may not have
        // received signal before os::killtree() failed.
        os::kill(pid, SIGTERM);
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
  }

  void reaped(pid_t pid, const Future<Option<int>>& status_)
  {
    terminated = true;

    // Stop checking the task.
    if (checker.get() != nullptr) {
      checker->stop();
    }

    // Stop health checking the task.
    if (healthChecker.get() != nullptr) {
      healthChecker->stop();
    }

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
      CHECK(WIFEXITED(status) || WIFSIGNALED(status))
        << "Unexpected wait status " << status;

      if (WSUCCEEDED(status)) {
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

    TaskStatus status = createTaskStatus(
        taskId.get(),
        taskState,
        None(),
        message);

    // Indicate that a kill occured due to a failing health check.
    if (killed && killedByHealthCheck) {
      status.set_healthy(false);
    }

    forward(status);

    Option<string> value = os::getenv("MESOS_HTTP_COMMAND_EXECUTOR");
    if (value.isSome() && value.get() == "1") {
      // For HTTP based executor, this is a fail safe in case the agent
      // doesn't send an ACK for the terminal update for some reason.
      delay(Seconds(60), self(), &Self::selfTerminate);
    } else {
      // For adapter based executor, this is a hack to ensure the status
      // update is sent to the agent before we exit the process. Without
      // this we may exit before libprocess has sent the data over the
      // socket. See MESOS-4111 for more details.
      delay(Seconds(1), self(), &Self::selfTerminate);
    }
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
    Try<std::list<os::ProcessTree>> trees =
      os::killtree(pid, SIGKILL, true, true);

    if (trees.isError()) {
      cerr << "Failed to kill the process tree rooted at pid "
           << pid << ": " << trees.error() << endl;

      // Process 'pid' may not have received signal before
      // os::killtree() failed. To make sure process 'pid' is reaped
      // we send SIGKILL directly.
      os::kill(pid, SIGKILL);
    } else {
      cout << "Killed the following process trees:\n" << stringify(trees.get())
           << endl;
    }
  }

  // Use this helper to create a status update from scratch, i.e., without
  // previously attached extra information like `data` or `check_status`.
  TaskStatus createTaskStatus(
      const TaskID& _taskId,
      const TaskState& state,
      const Option<TaskStatus::Reason>& reason = None(),
      const Option<string>& message = None())
  {
    TaskStatus status = protobuf::createTaskStatus(
        _taskId,
        state,
        UUID::random(),
        Clock::now().secs());

    status.mutable_executor_id()->CopyFrom(executorId);
    status.set_source(TaskStatus::SOURCE_EXECUTOR);

    if (reason.isSome()) {
      status.set_reason(reason.get());
    }

    if (message.isSome()) {
      status.set_message(message.get());
    }

    // TODO(alexr): Augment health information in a way similar to
    // `CheckStatusInfo`. See MESOS-6417 for more details.

    // If a check for the task has been defined, `check_status` field in each
    // task status must be set to a valid `CheckStatusInfo` message even if
    // there is no check status available yet.
    CHECK(taskData.isSome());
    if (taskData->taskInfo.has_check()) {
      CheckStatusInfo checkStatusInfo;
      checkStatusInfo.set_type(taskData->taskInfo.check().type());
      switch (taskData->taskInfo.check().type()) {
        case CheckInfo::COMMAND: {
          checkStatusInfo.mutable_command();
          break;
        }

        case CheckInfo::HTTP: {
          checkStatusInfo.mutable_http();
          break;
        }

        case CheckInfo::UNKNOWN: {
          CHECK_NE(CheckInfo::UNKNOWN, taskData->taskInfo.check().type());
          break;
        }
      }

      status.mutable_check_status()->CopyFrom(checkStatusInfo);
    }

    return status;
  }

  void forward(const TaskStatus& status)
  {
    Call call;
    call.set_type(Call::UPDATE);

    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(executorId);

    call.mutable_update()->mutable_status()->CopyFrom(status);

    // Capture the status update.
    unacknowledgedUpdates[UUID::fromBytes(status.uuid()).get()] = call.update();

    // Overwrite the last task status.
    lastTaskStatus = status;

    mesos->send(evolve(call));
  }

  void selfTerminate()
  {
    Option<string> value = os::getenv("MESOS_HTTP_COMMAND_EXECUTOR");
    if (value.isSome() && value.get() == "1") {
      // If we get here, that means HTTP based command executor does
      // not get the ACK for the terminal status update, let's exit
      // with non-zero status since this should not happen.
      EXIT(EXIT_FAILURE)
        << "Did not receive ACK for the terminal status update from the agent";
    } else {
      // For adapter based executor, the terminal status update should
      // have already been sent to the agent at this point, so we can
      // safely self terminate.
      terminate(self());
    }
  }

  enum State
  {
    CONNECTED,
    DISCONNECTED,
    SUBSCRIBED
  } state;

  struct TaskData
  {
    explicit TaskData(const TaskInfo& _taskInfo)
      : taskInfo(_taskInfo), acknowledged(false) {}

    TaskInfo taskInfo;

    // Indicates whether a status update acknowledgement
    // has been received for any status update.
    bool acknowledged;
  };

  // Once `TaskInfo` is received, it is cached for later access.
  Option<TaskData> taskData;

  // TODO(alexr): Introduce a state enum and document transitions,
  // see MESOS-5252.
  bool launched;
  bool killed;
  bool killedByHealthCheck;
  bool terminated;

  Option<Time> killGracePeriodStart;
  Option<Timer> killGracePeriodTimer;

  pid_t pid;
#ifdef __WINDOWS__
  HANDLE processHandle;
#endif
  Duration shutdownGracePeriod;
  Option<KillPolicy> killPolicy;
  Option<FrameworkInfo> frameworkInfo;
  Option<TaskID> taskId;
  string launcherDir;
  Option<string> rootfs;
  Option<string> sandboxDirectory;
  Option<string> workingDirectory;
  Option<string> user;
  Option<string> taskCommand;
  Option<Environment> taskEnvironment;
  Option<CapabilityInfo> capabilities;
  const FrameworkID frameworkId;
  const ExecutorID executorId;
  Owned<MesosBase> mesos;

  LinkedHashMap<UUID, Call::Update> unacknowledgedUpdates;

  Option<TaskStatus> lastTaskStatus;

  Owned<checks::Checker> checker;
  Owned<checks::HealthChecker> healthChecker;
};

} // namespace internal {
} // namespace mesos {


class Flags : public virtual flags::FlagsBase
{
public:
  Flags()
  {
    add(&Flags::rootfs,
        "rootfs",
        "The path to the root filesystem for the task");

    // The following flags are only applicable when a rootfs is
    // provisioned for this command.
    add(&Flags::sandbox_directory,
        "sandbox_directory",
        "The absolute path for the directory in the container where the\n"
        "sandbox is mapped to");

    add(&Flags::working_directory,
        "working_directory",
        "The working directory for the task in the container.");

    add(&Flags::user,
        "user",
        "The user that the task should be running as.");

    add(&Flags::task_command,
        "task_command",
        "If specified, this is the overrided command for launching the\n"
        "task (instead of the command from TaskInfo).");

    add(&Flags::task_environment,
        "task_environment",
        "If specified, this is a JSON-ified `Environment` protobuf that\n"
        "should be added to the executor's environment before launching\n"
        "the task.");

    add(&Flags::capabilities,
        "capabilities",
        "Capabilities the command can use.");

    add(&Flags::launcher_dir,
        "launcher_dir",
        "Directory path of Mesos binaries.",
        PKGLIBEXECDIR);

    // TODO(nnielsen): Add 'prefix' option to enable replacing
    // 'sh -c' with user specified wrapper.
  }

  Option<string> rootfs;
  Option<string> sandbox_directory;
  Option<string> working_directory;
  Option<string> user;
  Option<string> task_command;
  Option<Environment> task_environment;
  Option<mesos::CapabilityInfo> capabilities;
  string launcher_dir;
};


int main(int argc, char** argv)
{
  Flags flags;
  mesos::FrameworkID frameworkId;
  mesos::ExecutorID executorId;

  process::initialize();

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

  Owned<mesos::internal::CommandExecutor> executor(
      new mesos::internal::CommandExecutor(
          flags.launcher_dir,
          flags.rootfs,
          flags.sandbox_directory,
          flags.working_directory,
          flags.user,
          flags.task_command,
          flags.task_environment,
          flags.capabilities,
          frameworkId,
          executorId,
          shutdownGracePeriod));

  process::spawn(executor.get());
  process::wait(executor.get());
  executor.reset();

  // NOTE: We need to finalize libprocess, on Windows especially,
  // as any binary that uses the networking stack on Windows must
  // also clean up the networking stack before exiting.
  process::finalize(true);
  return EXIT_SUCCESS;
}
