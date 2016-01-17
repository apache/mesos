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

#include <string>

#include <mesos/mesos.hpp>
#include <mesos/executor.hpp>

#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/subprocess.hpp>
#include <process/reap.hpp>
#include <process/owned.hpp>

#include <stout/flags.hpp>
#include <stout/protobuf.hpp>
#include <stout/os.hpp>

#include "common/status_utils.hpp"

#include "docker/docker.hpp"
#include "docker/executor.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "messages/messages.hpp"

using namespace mesos;
using namespace process;

using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace docker {

const Duration DOCKER_INSPECT_DELAY = Milliseconds(500);
const Duration DOCKER_INSPECT_TIMEOUT = Seconds(5);

// Executor that is responsible to execute a docker container, and
// redirect log output to configured stdout and stderr files.
// Similar to the CommandExecutor, it is only responsible to launch
// one container and exits afterwards.
// The executor assumes that it is launched from the
// DockerContainerizer, which already sets up when launching the
// executor that ensures its kept running if the slave exits.
class DockerExecutorProcess : public ProtobufProcess<DockerExecutorProcess>
{
public:
  DockerExecutorProcess(
      const Owned<Docker>& docker,
      const string& containerName,
      const string& sandboxDirectory,
      const string& mappedDirectory,
      const Duration& stopTimeout,
      const string& healthCheckDir)
    : killed(false),
      killedByHealthCheck(false),
      healthPid(-1),
      healthCheckDir(healthCheckDir),
      docker(docker),
      containerName(containerName),
      sandboxDirectory(sandboxDirectory),
      mappedDirectory(mappedDirectory),
      stopTimeout(stopTimeout),
      stop(Nothing()),
      inspect(Nothing()) {}

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

  void disconnected(ExecutorDriver* driver)
  {
    cout << "Disconnected from the slave" << endl;
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

    TaskID taskId = task.task_id();

    cout << "Starting task " << taskId.value() << endl;

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
        None(),
        Subprocess::FD(STDOUT_FILENO),
        Subprocess::FD(STDERR_FILENO))
      .onAny(defer(
        self(),
        &Self::reaped,
        driver,
        taskId,
        lambda::_1));

    // Delay sending TASK_RUNNING status update until we receive
    // inspect output.
    inspect = docker->inspect(containerName, DOCKER_INSPECT_DELAY)
      .then(defer(self(), [=](const Docker::Container& container) {
        if (!killed) {
          TaskStatus status;
          status.mutable_task_id()->CopyFrom(taskId);
          status.set_state(TASK_RUNNING);
          status.set_data(container.output);
          if (container.ipAddress.isSome()) {
            // TODO(karya): Deprecated -- Remove after 0.25.0 has shipped.
            Label* label = status.mutable_labels()->add_labels();
            label->set_key("Docker.NetworkSettings.IPAddress");
            label->set_value(container.ipAddress.get());

            NetworkInfo* networkInfo =
              status.mutable_container_status()->add_network_infos();

            // TODO(CD): Deprecated -- Remove after 0.27.0.
            networkInfo->set_ip_address(container.ipAddress.get());

            NetworkInfo::IPAddress* ipAddress =
              networkInfo->add_ip_addresses();
            ipAddress->set_ip_address(container.ipAddress.get());
          }
          driver->sendStatusUpdate(status);
        }

        return Nothing();
      }));

    inspect.onReady(
        defer(self(), &Self::launchHealthCheck, containerName, task));
  }

  void killTask(ExecutorDriver* driver, const TaskID& taskId)
  {
    cout << "Killing docker task" << endl;
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

    if (run.isSome() && !killed) {
      // The docker daemon might still be in progress starting the
      // container, therefore we kill both the docker run process
      // and also ask the daemon to stop the container.

      // Making a mutable copy of the future so we can call discard.
      Future<Nothing>(run.get()).discard();
      stop = docker->stop(containerName, stopTimeout);
      killed = true;
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
    driver.get()->sendStatusUpdate(status);

    if (initiateTaskKill) {
      killedByHealthCheck = true;
      killTask(driver.get(), taskID);
    }
  }

private:
  void reaped(
      ExecutorDriver* _driver,
      const TaskID& taskId,
      const Future<Nothing>& run)
  {
    // Wait for docker->stop to finish, and best effort wait for the
    // inspect future to complete with a timeout.
    stop.onAny(defer(self(), [=](const Future<Nothing>&) {
      inspect
        .after(DOCKER_INSPECT_TIMEOUT, [=](const Future<Nothing>&) {
          inspect.discard();
          return inspect;
        })
        .onAny(defer(self(), [=](const Future<Nothing>&) {
          CHECK_SOME(driver);
          TaskState state;
          string message;
          if (!stop.isReady()) {
            state = TASK_FAILED;
            message = "Unable to stop docker container, error: " +
                      (stop.isFailed() ? stop.failure() : "future discarded");
          } else if (killed) {
            state = TASK_KILLED;
          } else if (!run.isReady()) {
            state = TASK_FAILED;
            message = "Docker container run error: " +
                      (run.isFailed() ?
                       run.failure() : "future discarded");
          } else {
            state = TASK_FINISHED;
          }

          TaskStatus taskStatus;
          taskStatus.mutable_task_id()->CopyFrom(taskId);
          taskStatus.set_state(state);
          taskStatus.set_message(message);
          if (killed && killedByHealthCheck) {
            taskStatus.set_healthy(false);
          }

          driver.get()->sendStatusUpdate(taskStatus);

          // A hack for now ... but we need to wait until the status update
          // is sent to the slave before we shut ourselves down.
          // TODO(tnachen): Remove this hack and also the same hack in the
          // command executor when we have the new HTTP APIs to wait until
          // an ack.
          os::sleep(Seconds(1));
          driver.get()->stop();
        }));
    }));
  }

  void launchHealthCheck(const string& containerName, const TaskInfo& task)
  {
    if (!killed && task.has_health_check()) {
      HealthCheck healthCheck = task.health_check();

      // Wrap the original health check command in "docker exec".
      if (healthCheck.has_command()) {
          CommandInfo command = healthCheck.command();

          // "docker exec" require docker version greater than 1.3.0.
          Try<Nothing> validateVersion =
            docker->validateVersion(Version(1, 3, 0));
          if (validateVersion.isError()) {
            cerr << "Unable to launch health process: "
                 << validateVersion.error() << endl;
            return;
          }

          vector<string> argv;
          argv.push_back(docker->getPath());
          argv.push_back("exec");
          argv.push_back(containerName);

          if (command.shell()) {
            if (!command.has_value()) {
              cerr << "Unable to launch health process: "
                   << "Shell command is not specified." << endl;
              return;
            }

            argv.push_back("sh");
            argv.push_back("-c");
            argv.push_back("\"");
            argv.push_back(command.value());
            argv.push_back("\"");
          } else {
            if (!command.has_value()) {
              cerr << "Unable to launch health process: "
                   << "Executable path is not specified." << endl;
              return;
            }

            argv.push_back(command.value());
            foreach (const string& argument, command.arguments()) {
              argv.push_back(argument);
            }
          }

          command.set_shell(true);
          command.clear_arguments();
          command.set_value(strings::join(" ", argv));
          healthCheck.mutable_command()->CopyFrom(command);
      } else {
          cerr << "Unable to launch health process: "
               << "Only command health check is supported now." << endl;
          return;
      }

      JSON::Object json = JSON::protobuf(healthCheck);

      // Launch the subprocess using 'exec' style so that quotes can
      // be properly handled.
      vector<string> argv;
      string path = path::join(healthCheckDir, "mesos-health-check");
      argv.push_back(path);
      argv.push_back("--executor=" + stringify(self()));
      argv.push_back("--health_check_json=" + stringify(json));
      argv.push_back("--task_id=" + task.task_id().value());

      string cmd = strings::join(" ", argv);
      cout << "Launching health check process: " << cmd << endl;

      Try<Subprocess> healthProcess =
        process::subprocess(
          path,
          argv,
          // Intentionally not sending STDIN to avoid health check
          // commands that expect STDIN input to block.
          Subprocess::PATH("/dev/null"),
          Subprocess::FD(STDOUT_FILENO),
          Subprocess::FD(STDERR_FILENO));

      if (healthProcess.isError()) {
        cerr << "Unable to launch health process: "
             << healthProcess.error() << endl;
      } else {
        healthPid = healthProcess.get().pid();

        cout << "Health check process launched at pid: "
             << stringify(healthPid) << endl;
      }
    }
  }

  bool killed;
  bool killedByHealthCheck;
  pid_t healthPid;
  string healthCheckDir;
  Owned<Docker> docker;
  string containerName;
  string sandboxDirectory;
  string mappedDirectory;
  Duration stopTimeout;
  Option<Future<Nothing>> run;
  Future<Nothing> stop;
  Future<Nothing> inspect;
  Option<ExecutorDriver*> driver;
};


class DockerExecutor : public Executor
{
public:
  DockerExecutor(
      const Owned<Docker>& docker,
      const string& container,
      const string& sandboxDirectory,
      const string& mappedDirectory,
      const Duration& stopTimeout,
      const string& healthCheckDir)
  {
    process = Owned<DockerExecutorProcess>(new DockerExecutorProcess(
        docker,
        container,
        sandboxDirectory,
        mappedDirectory,
        stopTimeout,
        healthCheckDir));

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
  Try<Nothing> load = flags.load(None(), &argc, &argv);

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  std::cout << stringify(flags) << std::endl;

  mesos::internal::logging::initialize(argv[0], flags, true); // Catch signals.

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

  if (flags.stop_timeout.isNone()) {
    cerr << flags.usage("Missing required option --stop_timeout") << endl;
    return EXIT_FAILURE;
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
      flags.stop_timeout.get(),
      flags.launcher_dir.get());

  mesos::MesosExecutorDriver driver(&executor);
  return driver.run() == mesos::DRIVER_STOPPED ? EXIT_SUCCESS : EXIT_FAILURE;
}
