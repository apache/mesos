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
#ifdef __WINDOWS__
#include <process/windows/jobobject.hpp>
#endif // __WINDOWS__

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

#ifdef __WINDOWS__
#include <stout/os/windows/jobobject.hpp>
#endif // __WINDOWS__

#include "checks/checker.hpp"
#include "checks/checks_runtime.hpp"
#include "checks/checks_types.hpp"
#include "checks/health_checker.hpp"

#include "common/http.hpp"
#include "common/parse.hpp"
#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"

#include "executor/v0_v1executor.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#ifdef ENABLE_LAUNCHER_SEALING
#include "linux/memfd.hpp"
#endif // ENABLE_LAUNCHER_SEALING

#include "logging/logging.hpp"

#include "messages/messages.hpp"

#include "slave/constants.hpp"
#include "slave/containerizer/mesos/constants.hpp"
#include "slave/containerizer/mesos/launch.hpp"

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

using mesos::slave::ContainerLaunchInfo;

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
      const Option<CapabilityInfo>& _effectiveCapabilities,
      const Option<CapabilityInfo>& _boundingCapabilities,
      const Option<string>& _ttySlavePath,
      const Option<ContainerLaunchInfo>& _taskLaunchInfo,
      const Option<vector<gid_t>> _taskSupplementaryGroups,
      const FrameworkID& _frameworkId,
      const ExecutorID& _executorId,
      const Duration& _shutdownGracePeriod)
    : ProcessBase(process::ID::generate("command-executor")),
      state(DISCONNECTED),
      taskData(None()),
      launched(false),
      killed(false),
      killedByHealthCheck(false),
      killedByMaxCompletionTimer(false),
      terminated(false),
      pid(None()),
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
      effectiveCapabilities(_effectiveCapabilities),
      boundingCapabilities(_boundingCapabilities),
      ttySlavePath(_ttySlavePath),
      taskLaunchInfo(_taskLaunchInfo),
      taskSupplementaryGroups(_taskSupplementaryGroups),
      frameworkId(_frameworkId),
      executorId(_executorId),
      lastTaskStatus(None()) {}

  ~CommandExecutor() override = default;

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
    LOG(INFO) << "Received " << event.type() << " event";

    switch (event.type()) {
      case Event::SUBSCRIBED: {
        LOG(INFO) << "Subscribed executor on "
                  << event.subscribed().slave_info().hostname();

        frameworkInfo = event.subscribed().framework_info();
        state = SUBSCRIBED;
        break;
      }

      case Event::LAUNCH: {
        launch(event.launch().task());
        break;
      }

      case Event::LAUNCH_GROUP: {
        LOG(ERROR) << "LAUNCH_GROUP event is not supported";
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
        const id::UUID uuid =
          id::UUID::fromBytes(event.acknowledged().uuid()).get();

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
        LOG(ERROR) << "Error: " << event.error().message();
        break;
      }

      case Event::HEARTBEAT: {
        break;
      }

      case Event::UNKNOWN: {
        LOG(WARNING) << "Received an UNKNOWN event and ignored";
        break;
      }
    }
  }

protected:
  void initialize() override
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

    LOG(INFO) << "Received check update '" << checkStatus << "'";

    // Use the previous task status to preserve all attached information.
    CHECK_SOME(lastTaskStatus);
    TaskStatus status = protobuf::createTaskStatus(
        lastTaskStatus.get(),
        id::UUID::random(),
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

    LOG(INFO) << "Received task health update, healthy: "
              << stringify(healthStatus.healthy());

    // Use the previous task status to preserve all attached information.
    CHECK_SOME(lastTaskStatus);
    TaskStatus status = protobuf::createTaskStatus(
        lastTaskStatus.get(),
        id::UUID::random(),
        Clock::now().secs(),
        None(),
        None(),
        None(),
        TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
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

  static Subprocess launchTaskSubprocess(
      const CommandInfo& command,
      const string& launcherDir,
      const Environment& environment,
      const Option<string>& user,
      const Option<string>& rootfs,
      const Option<string>& sandboxDirectory,
      const Option<string>& workingDirectory,
      const Option<CapabilityInfo>& effectiveCapabilities,
      const Option<CapabilityInfo>& boundingCapabilities,
      const Option<string>& ttySlavePath,
      const Option<ContainerLaunchInfo>& taskLaunchInfo,
      const Option<vector<gid_t>>& taskSupplementaryGroups)
  {
    // Prepare the flags to pass to the launch process.
    slave::MesosContainerizerLaunch::Flags launchFlags;

    ContainerLaunchInfo launchInfo;
    launchInfo.mutable_command()->CopyFrom(command);

#ifndef __WINDOWS__
    if (rootfs.isSome()) {
      // The command executor is responsible for chrooting into the
      // root filesystem and changing the user before exec-ing the
      // user process.
#ifdef __linux__
      if (geteuid() != 0) {
        ABORT("The command executor requires root with rootfs");
      }

      // Ensure that mount namespace of the executor is not affected by
      // changes in its task's namespace induced by calling `pivot_root`
      // as part of the task setup in mesos-containerizer binary.
      launchFlags.unshare_namespace_mnt = true;
#else
      ABORT("Not expecting root volume with non-linux platform");
#endif // __linux__

      launchInfo.set_rootfs(rootfs.get());

      CHECK_SOME(sandboxDirectory);

      launchInfo.set_working_directory(workingDirectory.isSome()
        ? workingDirectory.get()
        : sandboxDirectory.get());

      // TODO(jieyu): If the task has a rootfs, the executor itself will
      // be running as root. Its sandbox is owned by root as well. In
      // order for the task to be able to access to its sandbox, we need
      // to make sure the owner of the sandbox is 'user'. However, this
      // is still a workaround. The owner of the files downloaded by the
      // fetcher is still not correct (i.e., root).
      if (user.isSome()) {
        // NOTE: We only chown the sandbox directory (non-recursively).
        Try<Nothing> chown = os::chown(user.get(), os::getcwd(), false);
        if (chown.isError()) {
          ABORT("Failed to chown sandbox to user " +
                user.get() + ": " + chown.error());
        }
      }
    }
#endif // __WINDOWS__

    launchInfo.mutable_environment()->CopyFrom(environment);

    if (user.isSome()) {
      launchInfo.set_user(user.get());
    }

    if (effectiveCapabilities.isSome()) {
      launchInfo.mutable_effective_capabilities()->CopyFrom(
          effectiveCapabilities.get());
    }

    if (boundingCapabilities.isSome()) {
      launchInfo.mutable_bounding_capabilities()->CopyFrom(
          boundingCapabilities.get());
    }

    if (taskLaunchInfo.isSome()) {
      launchInfo.mutable_mounts()->CopyFrom(taskLaunchInfo->mounts());
      launchInfo.mutable_file_operations()->CopyFrom(
          taskLaunchInfo->file_operations());

      launchInfo.mutable_pre_exec_commands()->CopyFrom(
          taskLaunchInfo->pre_exec_commands());

      launchInfo.mutable_clone_namespaces()->CopyFrom(
          taskLaunchInfo->clone_namespaces());
    }

    if (taskSupplementaryGroups.isSome()) {
      foreach (gid_t gid, taskSupplementaryGroups.get()) {
        launchInfo.add_supplementary_groups(gid);
      }
    }

    launchFlags.launch_info = JSON::protobuf(launchInfo);

    // Determine the mesos containerizer binary depends on whether we
    // need to clone and seal it on linux.
    string initPath = path::join(launcherDir, MESOS_CONTAINERIZER);
#ifdef ENABLE_LAUNCHER_SEALING
    // Clone the launcher binary in memory for security concerns.
    Try<int_fd> memFd = memfd::cloneSealedFile(initPath);
    if (memFd.isError()) {
      ABORT(
          "Failed to clone a sealed file '" + initPath + "' in memory: " +
          memFd.error());
    }

    initPath = "/proc/self/fd/" + stringify(memFd.get());
#endif // ENABLE_LAUNCHER_SEALING

    // Fork the child using launcher.
    vector<string> argv(2);
    argv[0] = MESOS_CONTAINERIZER;
    argv[1] = MesosContainerizerLaunch::NAME;

    vector<process::Subprocess::ParentHook> parentHooks;
#ifdef __WINDOWS__
    parentHooks.emplace_back(Subprocess::ParentHook::CREATE_JOB());
    // Setting the "kill on close" job object limit ties the lifetime of the
    // task to that of the executor. This ensures that if the executor exits,
    // its task exits too.
    parentHooks.emplace_back(Subprocess::ParentHook(
        [](pid_t pid) { return os::set_job_kill_on_close_limit(pid); }));
#endif // __WINDOWS__

    vector<process::Subprocess::ChildHook> childHooks;
    if (ttySlavePath.isNone()) {
      childHooks.emplace_back(Subprocess::ChildHook::SETSID());
    }

    Try<Subprocess> s = subprocess(
        initPath,
        argv,
        Subprocess::FD(STDIN_FILENO),
        Subprocess::FD(STDOUT_FILENO),
        Subprocess::FD(STDERR_FILENO),
        &launchFlags,
        None(),
        None(),
        parentHooks,
        childHooks);

    if (s.isError()) {
      ABORT("Failed to launch task subprocess: " + s.error());
    }

    return *s;
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

    // Send initial TASK_STARTING update.
    TaskStatus starting = createTaskStatus(taskId.get(), TASK_STARTING);
    forward(starting);

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
    //
    // TODO(tillt): Consider logging in detail the original environment
    // variable source and overwriting source.
    //
    // TODO(tillt): Consider implementing a generic, reusable solution
    // for merging these environments. See MESOS-7299.
    //
    // Note that we can not use protobuf message merging as that could
    // cause duplicate keys in the resulting environment.
    hashmap<string, Environment::Variable> environment;

    foreachpair (const string& name, const string& value, os::environment()) {
      Environment::Variable variable;
      variable.set_name(name);
      variable.set_type(Environment::Variable::VALUE);
      variable.set_value(value);
      environment[name] = variable;
    }

    if (taskEnvironment.isSome()) {
      foreach (const Environment::Variable& variable,
               taskEnvironment->variables()) {
        // Skip overwriting if the variable is unresolved secret.
        if (variable.type() == Environment::Variable::SECRET) {
          continue;
        }
        const string& name = variable.name();
        if (environment.contains(name) &&
            environment[name].value() != variable.value()) {
          LOG(INFO) << "Overwriting environment variable '" << name << "'";
        }
        environment[name] = variable;
      }
    }

    if (command.has_environment()) {
      foreach (const Environment::Variable& variable,
               command.environment().variables()) {
        // Skip overwriting if the variable is unresolved secret.
        if (variable.type() == Environment::Variable::SECRET) {
          continue;
        }
        const string& name = variable.name();
        if (environment.contains(name) &&
            environment[name].value() != variable.value()) {
          LOG(INFO) << "Overwriting environment variable '" << name << "'";
        }
        environment[name] = variable;
      }
    }

    // Set the `MESOS_ALLOCATION_ROLE` environment variable for the task.
    // Please note that tasks are not allowed to mix resources allocated
    // to different roles, see MESOS-6636.
    Environment::Variable variable;
    variable.set_name("MESOS_ALLOCATION_ROLE");
    variable.set_type(Environment::Variable::VALUE);
    variable.set_value(task.resources().begin()->allocation_info().role());
    environment["MESOS_ALLOCATION_ROLE"] = variable;

    Environment launchEnvironment;
    foreachvalue (const Environment::Variable& variable, environment) {
      launchEnvironment.add_variables()->CopyFrom(variable);
    }

    // Setup timer for max_completion_time.
    if (task.max_completion_time().nanoseconds() > 0) {
      Duration duration = Nanoseconds(task.max_completion_time().nanoseconds());

      LOG(INFO) << "Task " << taskId.get() << " has a max completion time of "
                << duration;

      taskCompletionTimer = delay(
          duration,
          self(),
          &Self::taskCompletionTimeout,
          task.task_id(),
          duration);
    }

    LOG(INFO) << "Starting task " << taskId.get();

    Subprocess subprocess = launchTaskSubprocess(
        command,
        launcherDir,
        launchEnvironment,
        user,
        rootfs,
        sandboxDirectory,
        workingDirectory,
        effectiveCapabilities,
        boundingCapabilities,
        ttySlavePath,
        taskLaunchInfo,
        taskSupplementaryGroups);

    pid = subprocess.pid();

    LOG(INFO) << "Forked command at " << pid.get();

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

      const checks::runtime::Plain plainRuntime{namespaces, pid.get()};
      Try<Owned<checks::Checker>> _checker =
        checks::Checker::create(
            task.check(),
            launcherDir,
            defer(self(), &Self::taskCheckUpdated, taskId.get(), lambda::_1),
            taskId.get(),
            plainRuntime);

      if (_checker.isError()) {
        // TODO(alexr): Consider ABORT and return a TASK_FAILED here.
        LOG(ERROR) << "Failed to create checker: " << _checker.error();
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

      const checks::runtime::Plain plainRuntime{namespaces, pid.get()};
      Try<Owned<checks::HealthChecker>> _healthChecker =
        checks::HealthChecker::create(
            task.health_check(),
            launcherDir,
            defer(self(), &Self::taskHealthUpdated, lambda::_1),
            taskId.get(),
            plainRuntime);

      if (_healthChecker.isError()) {
        // TODO(gilbert): Consider ABORT and return a TASK_FAILED here.
        LOG(ERROR) << "Failed to create health checker: "
                   << _healthChecker.error();
      } else {
        healthChecker = _healthChecker.get();
      }
    }

    // Monitor this process.
    subprocess.status()
      .onAny(defer(self(), &Self::reaped, pid.get(), lambda::_1));

    TaskStatus status = createTaskStatus(taskId.get(), TASK_RUNNING);

    forward(status);
    launched = true;
  }

  void kill(const TaskID& _taskId, const Option<KillPolicy>& override = None())
  {
    // Cancel the taskCompletionTimer if it is set and ongoing.
    if (taskCompletionTimer.isSome()) {
      Clock::cancel(taskCompletionTimer.get());
      taskCompletionTimer = None();
    }

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

    LOG(INFO) << "Received kill for task " << _taskId.value()
              << " with grace period of " << gracePeriod;

    kill(_taskId, gracePeriod);
  }

  void shutdown()
  {
    LOG(INFO) << "Shutting down";

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
    } else {
      terminate(self());
    }
  }

private:
  void kill(const TaskID& _taskId, const Duration& gracePeriod)
  {
    if (terminated) {
      return;
    }

    // Terminate if a kill task request is received before the task is launched.
    // This can happen, for example, if `RunTaskMessage` has not been delivered.
    // See MESOS-8297.
    CHECK(launched) << "Terminating because kill task message has been"
                    << " received before the task has been launched";

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

      LOG(INFO) << "Rescheduling escalation to SIGKILL in " << remaining
                << " from now";

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

      if (!killedByMaxCompletionTimer &&
          protobuf::frameworkHasCapability(
              frameworkInfo.get(),
              FrameworkInfo::Capability::TASK_KILLING_STATE)) {
        TaskStatus status =
          createTaskStatus(taskId.get(), TASK_KILLING);

        forward(status);
      }

      // Stop checking the task.
      if (checker.get() != nullptr) {
        checker->pause();
      }

      // Stop health checking the task.
      if (healthChecker.get() != nullptr) {
        healthChecker->pause();
      }

      // Now perform signal escalation to begin killing the task.
      CHECK_SOME(pid);

      LOG(INFO) << "Sending SIGTERM to process tree at pid " << pid.get();

      Try<std::list<os::ProcessTree>> trees =
        os::killtree(pid.get(), SIGTERM, true, true);

      if (trees.isError()) {
        LOG(ERROR) << "Failed to kill the process tree rooted at pid "
                   << pid.get() << ": " << trees.error();

        // Send SIGTERM directly to process 'pid' as it may not have
        // received signal before os::killtree() failed.
        os::kill(pid.get(), SIGTERM);
      } else {
        LOG(INFO) << "Sent SIGTERM to the following process trees:\n"
                  << stringify(trees.get());
      }

      LOG(INFO) << "Scheduling escalation to SIGKILL in " << gracePeriod
                << " from now";

      killGracePeriodTimer =
        delay(gracePeriod, self(), &Self::escalated, gracePeriod);

      killGracePeriodStart = Clock::now();
      killed = true;
    }
  }

  void reaped(pid_t _pid, const Future<Option<int>>& status_)
  {
    terminated = true;

    // Stop checking the task.
    if (checker.get() != nullptr) {
      checker->pause();
    }

    // Stop health checking the task.
    if (healthChecker.get() != nullptr) {
      healthChecker->pause();
    }

    TaskState taskState;
    string message;

    if (killGracePeriodTimer.isSome()) {
      Clock::cancel(killGracePeriodTimer.get());
    }

    if (taskCompletionTimer.isSome()) {
      Clock::cancel(taskCompletionTimer.get());
      taskCompletionTimer = None();
    }

    Option<TaskStatus::Reason> reason = None();

    if (!status_.isReady()) {
      taskState = TASK_FAILED;
      message =
        "Failed to get exit status for Command: " +
        (status_.isFailed() ? status_.failure() : "future discarded");
    } else if (status_->isNone()) {
      taskState = TASK_FAILED;
      message = "Failed to get exit status for Command";
    } else {
      int status = status_->get();
      CHECK(WIFEXITED(status) || WIFSIGNALED(status))
        << "Unexpected wait status " << status;

      if (killedByMaxCompletionTimer) {
        taskState = TASK_FAILED;
        reason = TaskStatus::REASON_MAX_COMPLETION_TIME_REACHED;
      } else if (killed) {
        // Send TASK_KILLED if the task was killed as a result of
        // kill() or shutdown().
        taskState = TASK_KILLED;
      } else if (WSUCCEEDED(status)) {
        taskState = TASK_FINISHED;
      } else {
        taskState = TASK_FAILED;
      }

      message = "Command " + WSTRINGIFY(status);
    }

    LOG(INFO) << message << " (pid: " << _pid << ")";

    CHECK_SOME(taskId);

    TaskStatus status = createTaskStatus(
        taskId.get(),
        taskState,
        reason,
        message);

    // Indicate that a kill occurred due to a failing health check.
    if (killed && killedByHealthCheck) {
      // TODO(abudnik): Consider specifying appropriate status update reason,
      // saying that the task was killed due to a failing health check.
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

    CHECK_SOME(pid);

    LOG(INFO) << "Process " << pid.get() << " did not terminate after "
              << timeout << ", sending SIGKILL to process tree at "
              << pid.get();

    // TODO(nnielsen): Sending SIGTERM in the first stage of the
    // shutdown may leave orphan processes hanging off init. This
    // scenario will be handled when PID namespace encapsulated
    // execution is in place.
    Try<std::list<os::ProcessTree>> trees =
      os::killtree(pid.get(), SIGKILL, true, true);

    if (trees.isError()) {
      LOG(ERROR) << "Failed to kill the process tree rooted at pid "
                 << pid.get() << ": " << trees.error();

      // Process 'pid' may not have received signal before
      // os::killtree() failed. To make sure process 'pid' is reaped
      // we send SIGKILL directly.
      os::kill(pid.get(), SIGKILL);
    } else {
      LOG(INFO) << "Killed the following process trees:\n"
                << stringify(trees.get());
    }
  }


  void taskCompletionTimeout(const TaskID& taskId, const Duration& duration)
  {
    CHECK(!terminated);
    CHECK(!killed);

    LOG(INFO) << "Killing task " << taskId
              << " which exceeded its maximum completion time of " << duration;

    taskCompletionTimer = None();
    killedByMaxCompletionTimer = true;

    // Use a zero gracePeriod to kill the task.
    kill(taskId, Duration::zero());
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
        id::UUID::random(),
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
        case CheckInfo::TCP: {
          checkStatusInfo.mutable_tcp();
          break;
        }
        case CheckInfo::UNKNOWN: {
          LOG(FATAL) << "UNKNOWN check type is invalid";
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
    unacknowledgedUpdates[id::UUID::fromBytes(status.uuid()).get()] =
      call.update();

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
  bool killedByMaxCompletionTimer;

  bool terminated;

  Option<Time> killGracePeriodStart;
  Option<Timer> killGracePeriodTimer;
  Option<Timer> taskCompletionTimer;
  Option<pid_t> pid;
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
  Option<CapabilityInfo> effectiveCapabilities;
  Option<CapabilityInfo> boundingCapabilities;
  Option<string> ttySlavePath;
  Option<ContainerLaunchInfo> taskLaunchInfo;
  Option<vector<gid_t>> taskSupplementaryGroups;
  const FrameworkID frameworkId;
  const ExecutorID executorId;
  Owned<MesosBase> mesos;

  LinkedHashMap<id::UUID, Call::Update> unacknowledgedUpdates;

  Option<TaskStatus> lastTaskStatus;

  Owned<checks::Checker> checker;
  Owned<checks::HealthChecker> healthChecker;
};

} // namespace internal {
} // namespace mesos {


class Flags : public virtual mesos::internal::logging::Flags
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

    add(&Flags::effective_capabilities,
        "effective_capabilities",
        "Capabilities granted to the command at launch.");

    add(&Flags::bounding_capabilities,
        "bounding_capabilities",
        "The bounding set of capabilities the command can use.");

    add(&Flags::task_launch_info,
        "task_launch_info",
        "The launch info to run the task.");

    add(&Flags::tty_slave_path,
        "tty_slave_path",
        "A path to the slave end of the attached TTY if there is one.");

    add(&Flags::task_supplementary_groups,
        "task_supplementary_groups",
        "Comma-separated list of supplementary groups to run the task with.");

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
  Option<mesos::CapabilityInfo> effective_capabilities;
  Option<mesos::CapabilityInfo> bounding_capabilities;
  Option<string> tty_slave_path;
  Option<JSON::Object> task_launch_info;
  Option<vector<gid_t>> task_supplementary_groups;
  string launcher_dir;
};


int main(int argc, char** argv)
{
  mesos::FrameworkID frameworkId;
  mesos::ExecutorID executorId;

  Flags flags;

  // Load flags from command line.
  Try<flags::Warnings> load = flags.load(None(), &argc, &argv);

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  mesos::internal::logging::initialize(argv[0], true, flags); // Catch signals.

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
      EXIT(EXIT_FAILURE)
        << "Failed to parse value '" << value.get() << "'"
        << " of 'MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD': " << parse.error();
    }

    shutdownGracePeriod = parse.get();
  }

  Option<ContainerLaunchInfo> task_launch_info;
  if (flags.task_launch_info.isSome()) {
    Try<ContainerLaunchInfo> parse =
      protobuf::parse<ContainerLaunchInfo>(flags.task_launch_info.get());

    if (parse.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to parse task launch info: " << parse.error();
    }

    task_launch_info = parse.get();
  }

  process::initialize();

  Owned<mesos::internal::CommandExecutor> executor(
      new mesos::internal::CommandExecutor(
          flags.launcher_dir,
          flags.rootfs,
          flags.sandbox_directory,
          flags.working_directory,
          flags.user,
          flags.task_command,
          flags.task_environment,
          flags.effective_capabilities,
          flags.bounding_capabilities,
          flags.tty_slave_path,
          task_launch_info,
          flags.task_supplementary_groups,
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
