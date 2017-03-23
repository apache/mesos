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

#include <stdio.h>

#include <map>
#include <string>

#include <mesos/executor.hpp>
#include <mesos/mesos.hpp>

#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/error.hpp>
#include <stout/flags.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include <stout/os/killtree.hpp>

#include "checks/health_checker.hpp"

#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"

#include "docker/docker.hpp"
#include "docker/executor.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "messages/messages.hpp"

#include "slave/constants.hpp"

using namespace mesos;
using namespace process;

using std::cerr;
using std::cout;
using std::endl;
using std::map;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace docker {

const Duration DOCKER_INSPECT_DELAY = Milliseconds(500);
const Duration DOCKER_INSPECT_TIMEOUT = Seconds(5);

// Executor that is responsible to execute a docker container and
// redirect log output to configured stdout and stderr files. Similar
// to `CommandExecutor`, it launches a single task (a docker container)
// and exits after the task finishes or is killed. The executor assumes
// that it is launched from the `DockerContainerizer`.
class DockerExecutorProcess : public ProtobufProcess<DockerExecutorProcess>
{
public:
  DockerExecutorProcess(
      const Owned<Docker>& docker,
      const string& containerName,
      const string& sandboxDirectory,
      const string& mappedDirectory,
      const Duration& shutdownGracePeriod,
      const string& launcherDir,
      const map<string, string>& taskEnvironment,
      bool cgroupsEnableCfs)
    : ProcessBase(ID::generate("docker-executor")),
      killed(false),
      killedByHealthCheck(false),
      terminated(false),
      launcherDir(launcherDir),
      docker(docker),
      containerName(containerName),
      sandboxDirectory(sandboxDirectory),
      mappedDirectory(mappedDirectory),
      shutdownGracePeriod(shutdownGracePeriod),
      taskEnvironment(taskEnvironment),
      cgroupsEnableCfs(cgroupsEnableCfs),
      stop(Nothing()),
      inspect(Nothing()) {}

  virtual ~DockerExecutorProcess() {}

  void registered(
      ExecutorDriver* _driver,
      const ExecutorInfo& executorInfo,
      const FrameworkInfo& _frameworkInfo,
      const SlaveInfo& slaveInfo)
  {
    cout << "Registered docker executor on " << slaveInfo.hostname() << endl;
    driver = _driver;
    frameworkInfo = _frameworkInfo;
  }

  void reregistered(
      ExecutorDriver* driver,
      const SlaveInfo& slaveInfo)
  {
    cout << "Re-registered docker executor on " << slaveInfo.hostname() << endl;
  }

  void disconnected(ExecutorDriver* driver)
  {
    cout << "Disconnected from the agent" << endl;
  }

  void launchTask(ExecutorDriver* driver, const TaskInfo& task)
  {
    if (run.isSome()) {
      // TODO(alexr): Use `protobuf::createTaskStatus()`
      // instead of manually setting fields.
      TaskStatus status;
      status.mutable_task_id()->CopyFrom(task.task_id());
      status.set_state(TASK_FAILED);
      status.set_message(
          "Attempted to run multiple tasks using a \"docker\" executor");

      driver->sendStatusUpdate(status);
      return;
    }

    // Capture the TaskID.
    taskId = task.task_id();

    // Capture the kill policy.
    if (task.has_kill_policy()) {
      killPolicy = task.kill_policy();
    }

    cout << "Starting task " << taskId.get() << endl;

    CHECK(task.has_container());
    CHECK(task.has_command());

    CHECK(task.container().type() == ContainerInfo::DOCKER);

    Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
        task.container(),
        task.command(),
        containerName,
        sandboxDirectory,
        mappedDirectory,
        task.resources() + task.executor().resources(),
        cgroupsEnableCfs,
        taskEnvironment,
        None() // No extra devices.
    );

    if (runOptions.isError()) {
      // TODO(alexr): Use `protobuf::createTaskStatus()`
      // instead of manually setting fields.
      TaskStatus status;
      status.mutable_task_id()->CopyFrom(task.task_id());
      status.set_state(TASK_FAILED);
      status.set_message(
        "Failed to create docker run options: " + runOptions.error());

      driver->sendStatusUpdate(status);

      _stop();
      return;
    }

    // We're adding task and executor resources to launch docker since
    // the DockerContainerizer updates the container cgroup limits
    // directly and it expects it to be the sum of both task and
    // executor resources. This does leave to a bit of unaccounted
    // resources for running this executor, but we are assuming
    // this is just a very small amount of overcommit.
    run = docker->run(
        runOptions.get(),
        Subprocess::FD(STDOUT_FILENO),
        Subprocess::FD(STDERR_FILENO));

    run->onAny(defer(self(), &Self::reaped, lambda::_1));

    // Delay sending TASK_RUNNING status update until we receive
    // inspect output. Note that we store a future that completes
    // after the sending of the running update. This allows us to
    // ensure that the terminal update is sent after the running
    // update (see `reaped()`).
    inspect = docker->inspect(containerName, DOCKER_INSPECT_DELAY)
      .then(defer(self(), [=](const Docker::Container& container) {
        if (!killed) {
          containerPid = container.pid;

          // TODO(alexr): Use `protobuf::createTaskStatus()`
          // instead of manually setting fields.
          TaskStatus status;
          status.mutable_task_id()->CopyFrom(taskId.get());
          status.set_state(TASK_RUNNING);
          status.set_data(container.output);
          if (container.ipAddress.isSome()) {
            // TODO(karya): Deprecated -- Remove after 0.25.0 has shipped.
            Label* label = status.mutable_labels()->add_labels();
            label->set_key("Docker.NetworkSettings.IPAddress");
            label->set_value(container.ipAddress.get());

            NetworkInfo* networkInfo =
              status.mutable_container_status()->add_network_infos();

            // Copy the NetworkInfo if it is specified in the
            // ContainerInfo. A Docker container has at most one
            // NetworkInfo, which is validated in containerizer.
            if (task.container().network_infos().size() > 0) {
              networkInfo->CopyFrom(task.container().network_infos(0));
              networkInfo->clear_ip_addresses();
            }

            NetworkInfo::IPAddress* ipAddress = networkInfo->add_ip_addresses();
            ipAddress->set_ip_address(container.ipAddress.get());

            containerNetworkInfo = *networkInfo;
          }
          driver->sendStatusUpdate(status);
        }

        return Nothing();
      }));

    inspect.onFailed(defer(self(), [=](const string& failure) {
      cerr << "Failed to inspect container '" << containerName << "'"
           << ": " << failure << endl;

      // TODO(bmahler): This is fatal, try to shut down cleanly.
      // Since we don't have a container id, we can only discard
      // the run future.
    }));

    inspect.onReady(defer(self(), &Self::launchCheck, task));

    inspect.onReady(
        defer(self(), &Self::launchHealthCheck, containerName, task));
  }

  void killTask(ExecutorDriver* driver, const TaskID& taskId)
  {
    cout << "Received killTask for task " << taskId.value() << endl;

    // Using shutdown grace period as a default is backwards compatible
    // with the `stop_timeout` flag, deprecated in 1.0.
    Duration gracePeriod = shutdownGracePeriod;

    if (killPolicy.isSome() && killPolicy->has_grace_period()) {
      gracePeriod = Nanoseconds(killPolicy->grace_period().nanoseconds());
    }

    killTask(driver, taskId, gracePeriod);
  }

  void frameworkMessage(ExecutorDriver* driver, const string& data) {}

  void shutdown(ExecutorDriver* driver)
  {
    cout << "Shutting down" << endl;

    // Currently, 'docker->run' uses the reaper internally, hence we need
    // to account for the reap interval. We also leave a small buffer of
    // time to do the forced kill, otherwise the agent may destroy the
    // container before we can send `TASK_KILLED`.
    //
    // TODO(alexr): Remove `MAX_REAP_INTERVAL` once the reaper signals
    // immediately after the watched process has exited.
    Duration gracePeriod =
      shutdownGracePeriod - process::MAX_REAP_INTERVAL() - Seconds(1);

    // Since the docker executor manages a single task,
    // shutdown boils down to killing this task.
    //
    // TODO(bmahler): If a shutdown arrives after a kill task within
    // the grace period of the `KillPolicy`, we may need to escalate
    // more quickly (e.g. the shutdown grace period allotted by the
    // agent is smaller than the kill grace period).
    if (run.isSome()) {
      CHECK_SOME(taskId);
      killTask(driver, taskId.get(), gracePeriod);
    } else {
      driver->stop();
    }
  }

  void error(ExecutorDriver* driver, const string& message) {}

protected:
  void taskHealthUpdated(const TaskHealthStatus& healthStatus)
  {
    if (driver.isNone()) {
      return;
    }

    // This check prevents us from sending `TASK_RUNNING` updates
    // after the task has been transitioned to `TASK_KILLING`.
    if (killed || terminated) {
      return;
    }

    cout << "Received task health update, healthy: "
         << stringify(healthStatus.healthy()) << endl;

    // TODO(alexr): Use `protobuf::createTaskStatus()`
    // instead of manually setting fields.
    TaskStatus status;
    status.mutable_task_id()->CopyFrom(healthStatus.task_id());
    status.set_healthy(healthStatus.healthy());
    status.set_state(TASK_RUNNING);

    if (containerNetworkInfo.isSome()) {
      status.mutable_container_status()->add_network_infos()->CopyFrom(
          containerNetworkInfo.get());
    }

    driver.get()->sendStatusUpdate(status);

    if (healthStatus.kill_task()) {
      killedByHealthCheck = true;
      killTask(driver.get(), healthStatus.task_id());
    }
  }

private:
  void killTask(
      ExecutorDriver* driver,
      const TaskID& _taskId,
      const Duration& gracePeriod)
  {
    if (terminated) {
      return;
    }

    // TODO(alexr): If a kill is in progress, consider adjusting
    // the grace period if a new one is provided.

    // Issue the kill signal if the container is running
    // and we haven't killed it yet.
    if (run.isSome() && !killed) {
      // We have to issue the kill after 'docker inspect' has
      // completed, otherwise we may race with 'docker run'
      // and docker may not know about the container. Note
      // that we postpone setting `killed` because we don't
      // want to send TASK_KILLED without having actually
      // issued the kill.
      inspect
        .onAny(defer(self(), &Self::_killTask, _taskId, gracePeriod));
    }
  }

  void _killTask(const TaskID& taskId_, const Duration& gracePeriod)
  {
    CHECK_SOME(driver);
    CHECK_SOME(frameworkInfo);
    CHECK_SOME(taskId);
    CHECK_EQ(taskId_, taskId.get());

    if (!terminated && !killed) {
      // Because we rely on `killed` to determine whether
      // to send TASK_KILLED, we set `killed` only once the
      // kill is issued. If we set it earlier we're more
      // likely to send a TASK_KILLED without having ever
      // signaled the container. Note that in general it's
      // a race between signaling and the container
      // terminating with a non-zero exit status.
      killed = true;

      // Send TASK_KILLING if the framework can handle it.
      if (protobuf::frameworkHasCapability(
              frameworkInfo.get(),
              FrameworkInfo::Capability::TASK_KILLING_STATE)) {
        // TODO(alexr): Use `protobuf::createTaskStatus()`
        // instead of manually setting fields.
        TaskStatus status;
        status.mutable_task_id()->CopyFrom(taskId.get());
        status.set_state(TASK_KILLING);
        driver.get()->sendStatusUpdate(status);
      }

      // Stop health checking the task.
      if (checker.get() != nullptr) {
        checker->pause();
      }

      // TODO(bmahler): Replace this with 'docker kill' so
      // that we can adjust the grace period in the case of
      // a `KillPolicy` override.
      stop = docker->stop(containerName, gracePeriod);
    }
  }

  void reaped(const Future<Option<int>>& run)
  {
    terminated = true;

    // Stop health checking the task.
    if (checker.get() != nullptr) {
      checker->pause();
    }

    // In case the stop is stuck, discard it.
    stop.discard();

    // We wait for inspect to finish in order to ensure we send
    // the TASK_RUNNING status update.
    inspect
      .onAny(defer(self(), &Self::_reaped, run));

    // If the inspect takes too long we discard it to ensure we
    // don't wait forever, however in this case there may be no
    // TASK_RUNNING update.
    inspect
      .after(DOCKER_INSPECT_TIMEOUT, [=](const Future<Nothing>&) {
        inspect.discard();
        return inspect;
      });
  }

  void _reaped(const Future<Option<int>>& run)
  {
    TaskState state;
    string message;

    if (!run.isReady()) {
      // TODO(bmahler): Include the run command in the message.
      state = TASK_FAILED;
      message = "Failed to run docker container: " +
          (run.isFailed() ? run.failure() : "discarded");
    } else if (run->isNone()) {
      state = TASK_FAILED;
      message = "Failed to get exit status of container";
    } else {
      int status = run->get();
      CHECK(WIFEXITED(status) || WIFSIGNALED(status))
        << "Unexpected wait status " << status;

      if (WSUCCEEDED(status)) {
        state = TASK_FINISHED;
      } else if (killed) {
        // Send TASK_KILLED if the task was killed as a result of
        // kill() or shutdown(). Note that in general there is a
        // race between signaling the container and it terminating
        // uncleanly on its own.
        //
        // TODO(bmahler): Consider using the exit status to
        // determine whether the container was terminated via
        // our signal or terminated on its own.
        state = TASK_KILLED;
      } else {
        state = TASK_FAILED;
      }

      message = "Container " + WSTRINGIFY(status);
    }

    CHECK_SOME(taskId);

    // TODO(alexr): Use `protobuf::createTaskStatus()`
    // instead of manually setting fields.
    TaskStatus taskStatus;
    taskStatus.mutable_task_id()->CopyFrom(taskId.get());
    taskStatus.set_state(state);
    taskStatus.set_message(message);
    if (killed && killedByHealthCheck) {
      taskStatus.set_healthy(false);
    }

    CHECK_SOME(driver);
    driver.get()->sendStatusUpdate(taskStatus);

    _stop();
  }

  void _stop()
  {
    // A hack for now ... but we need to wait until the status update
    // is sent to the slave before we shut ourselves down.
    // TODO(tnachen): Remove this hack and also the same hack in the
    // command executor when we have the new HTTP APIs to wait until
    // an ack.
    os::sleep(Seconds(1));
    driver.get()->stop();
  }

  void launchCheck(const TaskInfo& task)
  {
    // TODO(alexr): Implement general checks support, see MESOS-7250.
    CHECK(!task.has_check()) << "Docker executor does not support checks yet";
  }

  void launchHealthCheck(const string& containerName, const TaskInfo& task)
  {
    // Bail out early if we have been already killed or if the task has no
    // associated health checks.
    //
    // TODO(alexr): Consider starting health checks even if we have
    // already been killed to ensure that tasks are health checked
    // while in their kill grace period.
    if (killed || !task.has_health_check()) {
      return;
    }

    HealthCheck healthCheck = task.health_check();

    // To make sure the health check runs in the same mount namespace
    // with the container, we wrap the original command in `docker exec`.
    if (healthCheck.has_command()) {
      // `docker exec` requires docker version greater than 1.3.0.
      Try<Nothing> validateVersion =
        docker->validateVersion(Version(1, 3, 0));

      if (validateVersion.isError()) {
        cerr << "Unable to launch health check process: "
             << validateVersion.error() << endl;
        return;
      }

      // Wrap the original health check command in `docker exec`.
      const CommandInfo& command = healthCheck.command();

      vector<string> commandArguments;
      commandArguments.push_back(docker->getPath());
      commandArguments.push_back("exec");
      commandArguments.push_back(containerName);

      if (command.shell()) {
        commandArguments.push_back("sh");
        commandArguments.push_back("-c");
        commandArguments.push_back("\"");
        commandArguments.push_back(command.value());
        commandArguments.push_back("\"");
      } else {
        commandArguments.push_back(command.value());

        foreach (const string& argument, command.arguments()) {
          commandArguments.push_back(argument);
        }
      }

      healthCheck.mutable_command()->set_shell(true);
      healthCheck.mutable_command()->clear_arguments();
      healthCheck.mutable_command()->set_value(
          strings::join(" ", commandArguments));
    }

    vector<string> namespaces;
    if (healthCheck.type() == HealthCheck::HTTP ||
        healthCheck.type() == HealthCheck::TCP) {
      // Make sure HTTP and TCP health checks are run
      // from the container's network namespace.
      namespaces.push_back("net");
    }

    Try<Owned<checks::HealthChecker>> _checker =
      checks::HealthChecker::create(
          healthCheck,
          launcherDir,
          defer(self(), &Self::taskHealthUpdated, lambda::_1),
          task.task_id(),
          containerPid,
          namespaces);

    if (_checker.isError()) {
      // TODO(gilbert): Consider ABORT and return a TASK_FAILED here.
      cerr << "Failed to create health checker: "
           << _checker.error() << endl;
    } else {
      checker = _checker.get();
    }
  }

  // TODO(alexr): Introduce a state enum and document transitions,
  // see MESOS-5252.
  bool killed;
  bool killedByHealthCheck;
  bool terminated;

  string launcherDir;
  Owned<Docker> docker;
  string containerName;
  string sandboxDirectory;
  string mappedDirectory;
  Duration shutdownGracePeriod;
  map<string, string> taskEnvironment;
  bool cgroupsEnableCfs;

  Option<KillPolicy> killPolicy;
  Option<Future<Option<int>>> run;
  Future<Nothing> stop;
  Future<Nothing> inspect;
  Option<ExecutorDriver*> driver;
  Option<FrameworkInfo> frameworkInfo;
  Option<TaskID> taskId;
  Owned<checks::HealthChecker> checker;
  Option<NetworkInfo> containerNetworkInfo;
  Option<pid_t> containerPid;
};


class DockerExecutor : public Executor
{
public:
  DockerExecutor(
      const Owned<Docker>& docker,
      const string& container,
      const string& sandboxDirectory,
      const string& mappedDirectory,
      const Duration& shutdownGracePeriod,
      const string& launcherDir,
      const map<string, string>& taskEnvironment,
      bool cgroupsEnableCfs)
  {
    process = Owned<DockerExecutorProcess>(new DockerExecutorProcess(
        docker,
        container,
        sandboxDirectory,
        mappedDirectory,
        shutdownGracePeriod,
        launcherDir,
        taskEnvironment,
        cgroupsEnableCfs));

    spawn(process.get());
  }

  virtual ~DockerExecutor()
  {
    terminate(process.get());
    wait(process.get());
  }

  virtual void registered(
      ExecutorDriver* driver,
      const ExecutorInfo& executorInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo)
  {
    dispatch(process.get(),
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
    dispatch(process.get(),
             &DockerExecutorProcess::reregistered,
             driver,
             slaveInfo);
  }

  virtual void disconnected(ExecutorDriver* driver)
  {
    dispatch(process.get(), &DockerExecutorProcess::disconnected, driver);
  }

  virtual void launchTask(ExecutorDriver* driver, const TaskInfo& task)
  {
    dispatch(process.get(), &DockerExecutorProcess::launchTask, driver, task);
  }

  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId)
  {
    dispatch(process.get(), &DockerExecutorProcess::killTask, driver, taskId);
  }

  virtual void frameworkMessage(ExecutorDriver* driver, const string& data)
  {
    dispatch(process.get(),
             &DockerExecutorProcess::frameworkMessage,
             driver,
             data);
  }

  virtual void shutdown(ExecutorDriver* driver)
  {
    dispatch(process.get(), &DockerExecutorProcess::shutdown, driver);
  }

  virtual void error(ExecutorDriver* driver, const string& data)
  {
    dispatch(process.get(), &DockerExecutorProcess::error, driver, data);
  }

private:
  Owned<DockerExecutorProcess> process;
};


} // namespace docker {
} // namespace internal {
} // namespace mesos {


int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  process::initialize();

  mesos::internal::docker::Flags flags;

  // Load flags from environment and command line.
  Try<flags::Warnings> load = flags.load(None(), &argc, &argv);

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  mesos::internal::logging::initialize(argv[0], flags, true); // Catch signals.

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  VLOG(1) << stringify(flags);

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (flags.docker.isNone()) {
    cerr << flags.usage("Missing required option --docker") << endl;
    return EXIT_FAILURE;
  }

  if (flags.container.isNone()) {
    cerr << flags.usage("Missing required option --container") << endl;
    return EXIT_FAILURE;
  }

  if (flags.sandbox_directory.isNone()) {
    cerr << flags.usage("Missing required option --sandbox_directory") << endl;
    return EXIT_FAILURE;
  }

  if (flags.mapped_directory.isNone()) {
    cerr << flags.usage("Missing required option --mapped_directory") << endl;
    return EXIT_FAILURE;
  }

  map<string, string> taskEnvironment;
  if (flags.task_environment.isSome()) {
    // Parse the string as JSON.
    Try<JSON::Object> json =
      JSON::parse<JSON::Object>(flags.task_environment.get());

    if (json.isError()) {
      cerr << flags.usage("Failed to parse --task_environment: " + json.error())
           << endl;
      return EXIT_FAILURE;
    }

    // Convert from JSON to map.
    foreachpair (
        const string& key,
        const JSON::Value& value,
        json->values) {
      if (!value.is<JSON::String>()) {
        cerr << flags.usage(
            "Value of key '" + key +
            "' in --task_environment is not a string")
             << endl;
        return EXIT_FAILURE;
      }

      // Save the parsed and validated key/value.
      taskEnvironment[key] = value.as<JSON::String>().value;
    }
  }

  // Get executor shutdown grace period from the environment.
  //
  // NOTE: We avoided introducing a docker executor flag for this
  // because the docker executor exits if it sees an unknown flag.
  // This makes it difficult to add or remove docker executor flags
  // that are unconditionally set by the agent.
  Duration shutdownGracePeriod =
    mesos::internal::slave::DEFAULT_EXECUTOR_SHUTDOWN_GRACE_PERIOD;
  Option<string> value = os::getenv("MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD");
  if (value.isSome()) {
    Try<Duration> parse = Duration::parse(value.get());
    if (parse.isError()) {
      cerr << "Failed to parse value '" << value.get() << "'"
           << " of 'MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD': " << parse.error();
      return EXIT_FAILURE;
    }

    shutdownGracePeriod = parse.get();
  }

  // If the deprecated flag is set, respect it and choose the bigger value.
  //
  // TODO(alexr): Remove this after the deprecation cycle (started in 1.0).
  if (flags.stop_timeout.isSome() &&
      flags.stop_timeout.get() > shutdownGracePeriod) {
    shutdownGracePeriod = flags.stop_timeout.get();
  }

  if (flags.launcher_dir.isNone()) {
    cerr << flags.usage("Missing required option --launcher_dir") << endl;
    return EXIT_FAILURE;
  }

  // The 2nd argument for docker create is set to false so we skip
  // validation when creating a docker abstraction, as the slave
  // should have already validated docker.
  Try<Owned<Docker>> docker = Docker::create(
      flags.docker.get(),
      flags.docker_socket.get(),
      false);

  if (docker.isError()) {
    cerr << "Unable to create docker abstraction: " << docker.error() << endl;
    return EXIT_FAILURE;
  }

  Owned<mesos::internal::docker::DockerExecutor> executor(
      new mesos::internal::docker::DockerExecutor(
          docker.get(),
          flags.container.get(),
          flags.sandbox_directory.get(),
          flags.mapped_directory.get(),
          shutdownGracePeriod,
          flags.launcher_dir.get(),
          taskEnvironment,
          flags.cgroups_enable_cfs));

  Owned<mesos::MesosExecutorDriver> driver(
      new mesos::MesosExecutorDriver(executor.get()));

  bool success = driver->run() == mesos::DRIVER_STOPPED;

  // NOTE: We need to delete the executor and driver before we call
  // `process::finalize` because the executor/driver will try to terminate
  // and wait on a libprocess actor in their destructor.
  driver.reset();
  executor.reset();

  // NOTE: We need to finalize libprocess, on Windows especially,
  // as any binary that uses the networking stack on Windows must
  // also clean up the networking stack before exiting.
  process::finalize(true);
  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
