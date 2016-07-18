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

#include <mesos/mesos.hpp>
#include <mesos/executor.hpp>

#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/subprocess.hpp>
#include <process/reap.hpp>
#include <process/owned.hpp>

#include <stout/error.hpp>
#include <stout/flags.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include <stout/os/killtree.hpp>

#include "common/status_utils.hpp"
#include "common/protobuf_utils.hpp"

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
      const string& healthCheckDir,
      const map<string, string>& taskEnvironment)
    : killed(false),
      killedByHealthCheck(false),
      terminated(false),
      healthPid(-1),
      healthCheckDir(healthCheckDir),
      docker(docker),
      containerName(containerName),
      sandboxDirectory(sandboxDirectory),
      mappedDirectory(mappedDirectory),
      shutdownGracePeriod(shutdownGracePeriod),
      taskEnvironment(taskEnvironment),
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

    // We're adding task and executor resources to launch docker since
    // the DockerContainerizer updates the container cgroup limits
    // directly and it expects it to be the sum of both task and
    // executor resources. This does leave to a bit of unaccounted
    // resources for running this executor, but we are assuming
    // this is just a very small amount of overcommit.
    run = docker->run(
        task.container(),
        task.command(),
        containerName,
        sandboxDirectory,
        mappedDirectory,
        task.resources() + task.executor().resources(),
        taskEnvironment,
        None(), // No extra devices.
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
  virtual void initialize()
  {
    install<TaskHealthStatus>(
        &Self::taskHealthUpdated,
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

    if (containerNetworkInfo.isSome()) {
      status.mutable_container_status()->add_network_infos()->CopyFrom(
          containerNetworkInfo.get());
    }

    driver.get()->sendStatusUpdate(status);

    if (initiateTaskKill) {
      killedByHealthCheck = true;
      killTask(driver.get(), taskID);
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

    // Cleanup health check process.
    //
    // TODO(bmahler): Consider doing this after the task has been
    // reaped, since a framework may be interested in health
    // information while the task is being killed (consider a
    // task that takes 30 minutes to be cleanly killed).
    if (healthPid != -1) {
      os::killtree(healthPid, SIGKILL);
      healthPid = -1;
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
        TaskStatus status;
        status.mutable_task_id()->CopyFrom(taskId.get());
        status.set_state(TASK_KILLING);
        driver.get()->sendStatusUpdate(status);
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
      CHECK(WIFEXITED(status) || WIFSIGNALED(status)) << status;

      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
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

    TaskStatus taskStatus;
    taskStatus.mutable_task_id()->CopyFrom(taskId.get());
    taskStatus.set_state(state);
    taskStatus.set_message(message);
    if (killed && killedByHealthCheck) {
      taskStatus.set_healthy(false);
    }

    CHECK_SOME(driver);
    driver.get()->sendStatusUpdate(taskStatus);

    // A hack for now ... but we need to wait until the status update
    // is sent to the slave before we shut ourselves down.
    // TODO(tnachen): Remove this hack and also the same hack in the
    // command executor when we have the new HTTP APIs to wait until
    // an ack.
    os::sleep(Seconds(1));
    driver.get()->stop();
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
    if (!healthCheck.has_command()) {
      cerr << "Unable to launch health process: "
           << "Only command health check is supported now" << endl;
      return;
    }

    // "docker exec" require docker version greater than 1.3.0.
    Try<Nothing> validateVersion =
      docker->validateVersion(Version(1, 3, 0));

    if (validateVersion.isError()) {
      cerr << "Unable to launch health process: "
           << validateVersion.error() << endl;
      return;
    }

    // Wrap the original health check command in "docker exec".
    const CommandInfo& command = healthCheck.command();
    if (!command.has_value()) {
      cerr << "Unable to launch health process: "
           << (command.shell() ? "Shell command" : "Executable path")
           << " is not specified" << endl;
      return;
    }

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

    JSON::Object json = JSON::protobuf(healthCheck);

    const string path = path::join(healthCheckDir, "mesos-health-check");

    // Launch the subprocess using 'exec' style so that quotes can
    // be properly handled.
    vector<string> checkerArguments;
    checkerArguments.push_back(path);
    checkerArguments.push_back("--executor=" + stringify(self()));
    checkerArguments.push_back("--health_check_json=" + stringify(json));
    checkerArguments.push_back("--task_id=" + task.task_id().value());

    cout << "Launching health check process: "
         << strings::join(" ", checkerArguments) << endl;

    Try<Subprocess> healthProcess =
      process::subprocess(
        path,
        checkerArguments,
        // Intentionally not sending STDIN to avoid health check
        // commands that expect STDIN input to block.
        Subprocess::PATH("/dev/null"),
        Subprocess::FD(STDOUT_FILENO),
        Subprocess::FD(STDERR_FILENO));

    if (healthProcess.isError()) {
      cerr << "Unable to launch health process: "
           << healthProcess.error() << endl;
      return;
    }

    healthPid = healthProcess.get().pid();

    cout << "Health check process launched at pid: "
         << stringify(healthPid) << endl;
  }

  // TODO(alexr): Introduce a state enum and document transitions,
  // see MESOS-5252.
  bool killed;
  bool killedByHealthCheck;
  bool terminated;

  pid_t healthPid;
  string healthCheckDir;
  Owned<Docker> docker;
  string containerName;
  string sandboxDirectory;
  string mappedDirectory;
  Duration shutdownGracePeriod;
  map<string, string> taskEnvironment;

  Option<KillPolicy> killPolicy;
  Option<Future<Option<int>>> run;
  Future<Nothing> stop;
  Future<Nothing> inspect;
  Option<ExecutorDriver*> driver;
  Option<FrameworkInfo> frameworkInfo;
  Option<TaskID> taskId;
  Option<NetworkInfo> containerNetworkInfo;
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
      const string& healthCheckDir,
      const map<string, string>& taskEnvironment)
  {
    process = Owned<DockerExecutorProcess>(new DockerExecutorProcess(
        docker,
        container,
        sandboxDirectory,
        mappedDirectory,
        shutdownGracePeriod,
        healthCheckDir,
        taskEnvironment));

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

  mesos::internal::docker::Flags flags;

  // Load flags from environment and command line.
  Try<flags::Warnings> load = flags.load(None(), &argc, &argv);

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  std::cout << stringify(flags) << std::endl;

  mesos::internal::logging::initialize(argv[0], flags, true); // Catch signals.

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  std::cout << stringify(flags) << std::endl;

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
        const std::string& key,
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

  mesos::internal::docker::DockerExecutor executor(
      docker.get(),
      flags.container.get(),
      flags.sandbox_directory.get(),
      flags.mapped_directory.get(),
      shutdownGracePeriod,
      flags.launcher_dir.get(),
      taskEnvironment);

  mesos::MesosExecutorDriver driver(&executor);
  return driver.run() == mesos::DRIVER_STOPPED ? EXIT_SUCCESS : EXIT_FAILURE;
}
