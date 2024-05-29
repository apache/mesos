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
#include <dirent.h>

#include <map>
#include <string>

#include <mesos/executor.hpp>
#include <mesos/mesos.hpp>

#include <process/io.hpp>
#include <process/collect.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>
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
#include <stout/os/which.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>
#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <stout/os/killtree.hpp>

#ifdef __WINDOWS__
#include <stout/os/windows/jobobject.hpp>
#endif // __WINDOWS__

#include "checks/checks_runtime.hpp"
#include "checks/health_checker.hpp"

#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"

#include "docker/docker.hpp"
#include "docker/executor.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "messages/flags.hpp"
#include "messages/messages.hpp"

#include "slave/constants.hpp"

namespace io = process::io;
namespace spec = mesos::internal::slave::cni::spec;

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
const Duration KILL_RETRY_INTERVAL = Seconds(5);

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
      const Option<ContainerDNSInfo>& defaultContainerDNS,
      bool cgroupsEnableCfs,
      const string& network_cni_plugins_dir,
      const string& network_cni_config_dir)      
    : ProcessBase(ID::generate("docker-executor")),
      killed(false),
      terminated(false),
      killedByHealthCheck(false),
      killedByTaskCompletionTimeout(false),
      launcherDir(launcherDir),
      docker(docker),
      containerName(containerName),
      sandboxDirectory(sandboxDirectory),
      mappedDirectory(mappedDirectory),
      shutdownGracePeriod(shutdownGracePeriod),
      taskEnvironment(taskEnvironment),
      defaultContainerDNS(defaultContainerDNS),
      cgroupsEnableCfs(cgroupsEnableCfs),
      network_cni_plugins_dir(network_cni_plugins_dir),
      network_cni_config_dir(network_cni_config_dir),      
      stop(Nothing()),
      inspect(Nothing()) {}

  ~DockerExecutorProcess() override {}

  void registered(
      ExecutorDriver* _driver,
      const ExecutorInfo& executorInfo,
      const FrameworkInfo& _frameworkInfo,
      const SlaveInfo& slaveInfo)
  {
    LOG(INFO) << "Registered docker executor on " << slaveInfo.hostname();
    driver = _driver;
    frameworkInfo = _frameworkInfo;
  }

  void reregistered(
      ExecutorDriver* driver,
      const SlaveInfo& slaveInfo)
  {
    LOG(INFO) << "Re-registered docker executor on " << slaveInfo.hostname();
  }

  void disconnected(ExecutorDriver* driver)
  {
    LOG(INFO) << "Disconnected from the agent";
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

    // Setup timer for max_completion_time.
    if (task.max_completion_time().nanoseconds() > 0) {
      Duration duration = Nanoseconds(task.max_completion_time().nanoseconds());

      LOG(INFO) << "Task " << taskId.get() << " has a max completion time of "
                << duration;

      taskCompletionTimer = delay(
          duration,
          self(),
          &Self::taskCompletionTimeout,
          driver,
          task.task_id(),
          duration);
    }

    LOG(INFO) << "Starting task " << taskId.get();

    // Send initial TASK_STARTING update.
    // TODO(alexr): Use `protobuf::createTaskStatus()`
    // instead of manually setting fields.
    TaskStatus starting;
    starting.mutable_task_id()->CopyFrom(task.task_id());
    starting.set_state(TASK_STARTING);
    driver->sendStatusUpdate(starting);

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
        None(), // No extra devices.
        defaultContainerDNS,
        task.limits());

    if (runOptions.isError()) {
      // TODO(alexr): Use `protobuf::createTaskStatus()`
      // instead of manually setting fields.
      TaskStatus status;
      status.mutable_task_id()->CopyFrom(task.task_id());
      status.set_state(TASK_FAILED);
      status.set_message(
        "Failed to create docker run options: " + runOptions.error());

      LOG(ERROR) << status.message();

      driver->sendStatusUpdate(status);

      // This is a fail safe in case the agent doesn't send an ACK for
      // the terminal update for some reason.
      delay(Seconds(60), self(), &Self::_stop);
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

    // Since the Docker daemon might hang, we have to retry the inspect command.
    auto inspectLoop = loop(
        self(),
        [=]() {
          return await(
              docker->inspect(containerName, DOCKER_INSPECT_DELAY)
                .after(
                    DOCKER_INSPECT_TIMEOUT,
                    [=](Future<Docker::Container> future) {
                      LOG(WARNING) << "Docker inspect timed out after "
                                   << DOCKER_INSPECT_TIMEOUT
                                   << " for container "
                                   << "'" << containerName << "'";

                      // We need to clean up the hanging Docker CLI process.
                      // Discarding the inspect future triggers a callback in
                      // the Docker library that kills the subprocess and
                      // transitions the future.
                      future.discard();
                      return future;
                    }));
        },
        [](const Future<Docker::Container>& future)
            -> Future<ControlFlow<Docker::Container>> {
          if (future.isReady()) {
            return Break(future.get());
          }
          if (future.isFailed()) {
            return Failure(future.failure());
          }
          return Continue();
        });

    // Delay sending TASK_RUNNING status update until we receive
    // inspect output. Note that we store a future that completes
    // after the sending of the running update. This allows us to
    // ensure that the terminal update is sent after the running
    // update (see `reaped()`).
    inspect =
      inspectLoop.then(defer(self(), [=](const Docker::Container& container) {
        if (!killed) {
          containerPid = container.pid;
          containerId = container.id;

          // TODO(alexr): Use `protobuf::createTaskStatus()`
          // instead of manually setting fields.
          TaskStatus status;
          status.mutable_task_id()->CopyFrom(taskId.get());
          status.set_state(TASK_RUNNING);
          status.set_data(container.output);
          if (container.ipAddress.isSome() ||
              container.ip6Address.isSome()) {
            NetworkInfo* networkInfo =
              status.mutable_container_status()->add_network_infos();

            // Copy the NetworkInfo if it is specified in the
            // ContainerInfo. A Docker container has at most one
            // NetworkInfo, which is validated in containerizer.
            if (task.container().network_infos().size() > 0) {
              networkInfo->CopyFrom(task.container().network_infos(0));
              networkInfo->clear_ip_addresses();
            }

            auto setIPAddresses = [=](const string& ip, bool ipv6) {
              NetworkInfo::IPAddress* ipAddress =
                networkInfo->add_ip_addresses();

              ipAddress->set_ip_address(ip);

              // NOTE: By default the protocol is set to IPv4 and therefore
              // we explicitly set the protocol only for an IPv6 address.
              if (ipv6) {
                ipAddress->set_protocol(NetworkInfo::IPv6);
              }
            };

            bool useCNI = false;
#ifdef __linux__  
            if (network_cni_config_dir.size() > 0 && network_cni_plugins_dir.size() > 0) {
              Try<std::tuple<Subprocess, std::string>> cni = attachCNI(task);
              if (!cni.isError()) {
                Subprocess cniSub = std::get<0>(cni.get());
                std::string cniNetworkName = std::get<1>(cni.get());
            
                WIFEXITED(cniSub.status().get().get());
                Try<std::string> cniIP = attachCNISuccess(cniSub);
                if (cniIP.isError()) {
                  LOG(ERROR) << cniIP.error();
                } else {
                  useCNI = true;
                  setIPAddresses(cniIP.get(), false);
                }
              }
            }
#endif
            if (container.ipAddress.isSome() && !useCNI) {            
              setIPAddresses(container.ipAddress.get(), false);
            }

            if (container.ip6Address.isSome()) {
              setIPAddresses(container.ip6Address.get(), true);
            }

            containerNetworkInfo = *networkInfo;
          }
          driver->sendStatusUpdate(status);
        }

        // This is a workaround for the Docker issue below:
        // https://github.com/moby/moby/issues/33820
        // Due to this issue, Docker daemon can fail to catch a container exit,
        // which will lead to the `docker run` command that we execute in this
        // executor never returning although the container has already exited.
        // To workaround this issue, here we reap the container process directly
        // so we will be notified when the container exits.
        //
        // The issue has only been reported on Linux, so it's not clear if
        // Windows also has this issue. Regardless, we don't use this workaround
        // for Windows, because the pid legitimately might not exist. For
        // example, if the container is running in Hyper-V isolation, the pid
        // will only exist in the guest OS.
#ifndef __WINDOWS__
        if (container.pid.isSome()) {
          process::reap(container.pid.get())
            .then(defer(self(), [=](const Option<int>& status) {
              // We cannot get the actual exit status of the container
              // process since it is not a child process of this executor,
              // so here `status` must be `None`.
              CHECK_NONE(status);

              // There will be a race between the method `reaped` and this
              // lambda; ideally when the Docker issue mentioned above does
              // not occur, `reaped` will be invoked (i.e., the `docker run`
              // command returns) to get the actual exit status of the
              // container, so here we wait 60 seconds for `reaped` to be
              // invoked. If `reaped` is not invoked within the timeout, that
              // means we hit that Docker issue.
              delay(
                  Seconds(60),
                  self(),
                  &Self::reapedContainer,
                  container.pid.get());

              return Nothing();
            }));
        } else {
          // This is the case that the container process has already exited,
          // Similar to the above case, let's wait 60 seconds for `reaped`
          // to be invoked.
          delay(
              Seconds(60),
              self(),
              &Self::reapedContainer,
              None());
        }
#endif // __WINDOWS__

        return Nothing();
      }));

    inspect.onFailed(defer(self(), [=](const string& failure) {
      LOG(ERROR) << "Failed to inspect container '" << containerName << "'"
                 << ": " << failure;

      // TODO(bmahler): This is fatal, try to shut down cleanly.
      // Since we don't have a container id, we can only discard
      // the run future.
    }));

    inspect.onReady(defer(self(), &Self::launchCheck, task));

    inspect.onReady(
        defer(self(), &Self::launchHealthCheck, containerName, task));
  }

  void killTask(
      ExecutorDriver* driver,
      const TaskID& taskId,
      const Option<KillPolicy>& killPolicyOverride = None())
  {
    string overrideMessage = "";
    if (killPolicyOverride.isSome() && killPolicyOverride->has_grace_period()) {
      Duration gracePeriodDuration =
        Nanoseconds(killPolicyOverride->grace_period().nanoseconds());

      overrideMessage =
        " with grace period override of " + stringify(gracePeriodDuration);
    }

    LOG(INFO) << "Received killTask" << overrideMessage
              << " for task " << taskId.value();

    // Using shutdown grace period as a default is backwards compatible
    // with the `stop_timeout` flag, deprecated in 1.0.
    Duration gracePeriod = shutdownGracePeriod;

    if (killPolicyOverride.isSome() && killPolicyOverride->has_grace_period()) {
      gracePeriod =
        Nanoseconds(killPolicyOverride->grace_period().nanoseconds());
    } else if (killPolicy.isSome() && killPolicy->has_grace_period()) {
      gracePeriod = Nanoseconds(killPolicy->grace_period().nanoseconds());
    }

    killTask(driver, taskId, gracePeriod);
  }

  void taskCompletionTimeout(
      ExecutorDriver* driver, const TaskID& taskId, const Duration& duration)
  {
    if (killed) {
      return;
    }

    if (terminated) {
      return;
    }

    LOG(INFO) << "Killing task " << taskId
              << " which exceeded its maximum completion time of " << duration;

    taskCompletionTimer = None();
    killedByTaskCompletionTimeout = true;
    killed = true;

    // Use a zero grace period to kill the task, in order to ignore the
    // `KillPolicy`.
    killTask(driver, taskId, Duration::zero());
  }

  void frameworkMessage(ExecutorDriver* driver, const string& data) {}

  void shutdown(ExecutorDriver* driver)
  {
    LOG(INFO) << "Shutting down";

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

    LOG(INFO) << "Received task health update, healthy: "
              << stringify(healthStatus.healthy());

    // TODO(alexr): Use `protobuf::createTaskStatus()`
    // instead of manually setting fields.
    TaskStatus status;
    status.mutable_task_id()->CopyFrom(healthStatus.task_id());
    status.set_healthy(healthStatus.healthy());
    status.set_state(TASK_RUNNING);
    status.set_reason(TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED);

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

    // Cancel the taskCompletionTimer if it is set and ongoing.
    if (taskCompletionTimer.isSome()) {
      Clock::cancel(taskCompletionTimer.get());
      taskCompletionTimer = None();
    }

    // Terminate if a kill task request is received before the task is launched.
    // This can happen, for example, if `RunTaskMessage` has not been delivered.
    // See MESOS-8297.
    CHECK_SOME(run) << "Terminating because kill task message has been"
                    << " received before the task has been launched";

    // TODO(alexr): If a kill is in progress, consider adjusting
    // the grace period if a new one is provided.

    // Issue the kill signal if there was an attempt to launch the container.
    if (run.isSome()) {
      // We have to issue the kill after 'docker inspect' has
      // completed, otherwise we may race with 'docker run'
      // and docker may not know about the container. Note
      // that we postpone setting `killed` because we don't
      // want to send TASK_KILLED without having actually
      // issued the kill.
      inspect
        .onAny(defer(self(), &Self::_killTask, _taskId, gracePeriod));

      // If the inspect takes too long we discard it to ensure we
      // don't wait forever, however in this case there may be no
      // TASK_RUNNING update.
      inspect
        .after(DOCKER_INSPECT_TIMEOUT, [=](const Future<Nothing>&) {
          inspect.discard();
          return inspect;
        });
    }
  }

  void _killTask(const TaskID& taskId_, const Duration& gracePeriod)
  {
    CHECK_SOME(driver);
    CHECK_SOME(frameworkInfo);
    CHECK_SOME(taskId);
    CHECK_EQ(taskId_, taskId.get());

    if (!terminated) {
      // Once the task has been transitioned to `killed`,
      // there is no way back, even if the kill attempt
      // failed. This also allows us to send TASK_KILLING
      // only once, regardless of how many kill attempts
      // have been made.
      //
      // Because we rely on `killed` to determine whether
      // to send TASK_KILLED, we set `killed` only once the
      // kill is issued. If we set it earlier we're more
      // likely to send a TASK_KILLED without having ever
      // signaled the container. Note that in general it's
      // a race between signaling and the container
      // terminating with a non-zero exit status.
      if (!killed) {
        killed = true;

#ifdef __linux__  
        // detach container network
        detachCNI();
#endif        

        // Send TASK_KILLING if task is not killed by completion timeout and
        // the framework can handle it.
        if (!killedByTaskCompletionTimeout &&
            protobuf::frameworkHasCapability(
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
      }

      // If a previous attempt to stop a Docker container is still in progress,
      // we need to kill the hanging Docker CLI subprocess. Discarding this
      // future triggers a callback in the Docker library that kills the
      // subprocess.
      if (stop.isPending()) {
        LOG(WARNING) << "Previous docker stop has not terminated yet"
                     << " for container '" << containerName << "'";
        stop.discard();
      }

      // TODO(bmahler): Replace this with 'docker kill' so
      // that we can adjust the grace period in the case of
      // a `KillPolicy` override.
      //
      // NOTE: `docker stop` may or may not finish. Our behaviour is to give
      // the subprocess a chance to finish until next time `_killtask` is
      // invoked. Also, invoking `docker stop` might be unsuccessful, in which
      // case the container most probably does not receive the signal.
      // In any case we should allow schedulers to retry the kill operation or,
      // if the kill was initiated by a failing health check, retry ourselves.
      // We do not bail out nor stop retrying to avoid sending a terminal
      // status update while the container might still be running.
      stop = docker->stop(containerName, gracePeriod);

      if (killedByHealthCheck) {
        stop
          .after(KILL_RETRY_INTERVAL, defer(self(), [=](Future<Nothing>) {
            LOG(INFO) << "Retrying to kill task";
            _killTask(taskId_, gracePeriod);
            return stop;
          }));
      }

      stop.onFailed(defer(self(), [=](const string& failure) {
        LOG(ERROR) << "Failed to stop container '" << containerName << "'"
                   << ": " << failure;

        if (killedByHealthCheck) {
          LOG(INFO) << "Retrying to kill task in " << KILL_RETRY_INTERVAL;
          delay(
              KILL_RETRY_INTERVAL,
              self(),
              &Self::_killTask,
              taskId_,
              gracePeriod);
        }
      }));
    }
  }

  void reapedContainer(Option<pid_t> pid)
  {
    // Do nothing if the method `reaped` has already been invoked.
    if (terminated) {
      return;
    }

    // This means the Docker issue mentioned in `launchTask` occurs.
    LOG(WARNING) << "The container process"
                 << (pid.isSome() ? " (pid: " + stringify(pid.get()) + ")" : "")
                 << " has exited, but Docker daemon failed to catch it.";

    reaped(None());
  }

  void reaped(const Future<Option<int>>& run)
  {
    if (terminated) {
      return;
    }

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
    Option<TaskStatus::Reason> reason = None();
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

      if (killedByTaskCompletionTimeout) {
        state = TASK_FAILED;
        reason = TaskStatus::REASON_MAX_COMPLETION_TIME_REACHED;
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
      } else if (WSUCCEEDED(status)) {
        state = TASK_FINISHED;
      } else {
        state = TASK_FAILED;
      }

      message = "Container " + WSTRINGIFY(status);
    }

#ifdef __linux__  
    // detach container network
    detachCNI();
#endif    

    LOG(INFO) << message;

    CHECK_SOME(taskId);

    // TODO(alexr): Use `protobuf::createTaskStatus()`
    // instead of manually setting fields.
    TaskStatus taskStatus;
    taskStatus.mutable_task_id()->CopyFrom(taskId.get());
    taskStatus.set_state(state);
    taskStatus.set_message(message);

    if (killed && killedByHealthCheck) {
      // TODO(abudnik): Consider specifying appropriate status update reason,
      // saying that the task was killed due to a failing health check.
      taskStatus.set_healthy(false);
    }

    if (reason.isSome()) {
      taskStatus.set_reason(reason.get());
    }

    CHECK_SOME(driver);
    driver.get()->sendStatusUpdate(taskStatus);

    // This is a fail safe in case the agent doesn't send an ACK for
    // the terminal update for some reason.
    delay(Seconds(60), self(), &Self::_stop);
  }

  void _stop()
  {
    driver.get()->stop();
  }

    // Return the content of the CNI plugin config file and return it as json object
  Try<JSON::Object> getNetworkConfigJSON(const string& fname) 
  { 
    Try<string> read = os::read(fname);
    if (read.isError()) {
      return Error("Cannot read CNI plugin file: " + read.error());
    }

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(read.get());
    if (parse.isError()) {
      return Error("Cannot parse CNI plugin file." + parse.error());
    }

    return parse;
  }

  // Search the config file of the network in the path
  Try<std::string> getNetworkConfigFile(const string& network, const string& path) 
  { 
    struct dirent *ent;    
    DIR *dir;
    JSON::Object parse;

    // path can include mutiple path's seperated by ":".
    char *sPath = strtok(const_cast<char*>(path.c_str()), ":"); 
    while (sPath != NULL) {
      if ((dir = opendir(sPath)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
          if (ent->d_type != DT_REG || !strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
          }

          std::string fname = string(sPath) + "/" + string(ent->d_name);
          Try<string> read = os::read(fname);
          if (read.isError()) {
            LOG(ERROR) << "Cannot read CNI network config file: " <<  fname;
            continue;
          }

          Try<JSON::Object> parse = JSON::parse<JSON::Object>(read.get());
          if (parse.isError()) {
            LOG(ERROR) << "Cannot parse CNI network config file.";
            continue;
          }

          Result<JSON::String> name = parse->at<JSON::String>("name");
          if (!name.isSome()) {
            return Error(
                "Cannot determine the 'name' of the CNI network for this "
                "configuration " +
                (name.isNone() ? "'" : ("': " + name.error())));
          }

          // Verify the configuration is for this network
          if (network == name->value) {
            return fname;
          }
        }
        closedir(dir);
      }
      sPath = strtok(NULL, ":");
    }
    return Error("Cannot find a CNI plugin config for this network name: " + network);
  }

  // attach CNI at the container
  Try<std::tuple<Subprocess, std::string>> attachCNI(const TaskInfo& task) 
  {
    container = task.container();
    return commandCNI(container, "ADD");
  }

  // detach CNI from the container
  Try<std::tuple<Subprocess, std::string>> detachCNI() 
  {
    return commandCNI(container, "DEL");
  }

  // function to attach or detach CNI from/to the container
  Try<std::tuple<Subprocess, std::string>> commandCNI(
    const mesos::ContainerInfo& container, 
    const string command) 
  {
    // Loop over all network objects
    int ifIndex = 1;
    foreach (const mesos::NetworkInfo& networkInfo, container.network_infos()) {
      if (!networkInfo.has_name()) {
        continue;
      }

      const string networkName = networkInfo.name();
      const string ifName = "eth" + stringify(ifIndex++);

      // Find the right plugin config file of the networkname
      Try<std::string> networkConfigFile = getNetworkConfigFile(networkName, network_cni_config_dir);
      if (networkConfigFile.isError()) {
        LOG(ERROR) << "Could not find the CNI plugin to use for network " 
                   <<  networkName 
                   << ". Deactivate CNI.";
        continue;
      }  

      Try<JSON::Object> networkConfigJSON = getNetworkConfigJSON(networkConfigFile.get());
      if (networkConfigJSON.isError()) {
        LOG(ERROR) << "Could not get valid CNI configuration for network '" 
                   << networkName  << "': " << networkConfigJSON.error();
        continue;
      }    

      // Invoke CNI plugin to get the plugin type
      Result<JSON::String> cniPluginType = networkConfigJSON->at<JSON::String>("type");
      if (cniPluginType.isError()) {
        LOG(ERROR) << cniPluginType.error();
        continue;
      }

      if (!cniPluginType.isSome()) {
        LOG(ERROR) << "Could not find CNI plugin type to use for network '" <<  networkName;
        continue;
      }

      map<string, string> environment;
      environment["CNI_COMMAND"] = command;
      environment["CNI_CONTAINERID"] = stringify(containerId);
      environment["CNI_PATH"] = network_cni_plugins_dir;
      environment["CNI_NETNS"] = "/proc/" + std::to_string(containerPid.get()) + "/ns/net";
      environment["CNI_IFNAME"] = ifName;

      // set the PATH in case the plugin need to use iptables
      Option<string> value = os::getenv("PATH");
      if (value.isSome()) {
        environment["PATH"] = value.get();
      } else {
        environment["PATH"] = os::host_default_path();
      }    

      // Invoke the CNI plugin.
      Option<string> plugin = os::which(
          cniPluginType->value,
          network_cni_plugins_dir);

      if (plugin.isNone()) {
        LOG(ERROR) << "Cannot load CNI plugin: " << cniPluginType->value;
        continue;
      }

      // attach cni plugin
      if (command.compare("ADD") == 0) {
        LOG(INFO) << "Invoking CNI plugin '" << plugin.get()
                  << "' to attach container " << containerId
                  << " to network '" << networkName << "'";
      } else {
        // detach cni plugin
        LOG(INFO) << "Invoking CNI plugin '" << plugin.get()
                  << "' to dettach container " << containerId
                  << " from network '" << networkName << "'";
      }
      LOG(INFO) << "CNI_COMMAND: " << environment["CNI_COMMAND"];
      LOG(INFO) << "CNI_CONTAINERID: " << environment["CNI_CONTAINERID"];
      LOG(INFO) << "CNI_NETNS: " << environment["CNI_NETNS"];
      LOG(INFO) << "CNI_PATH: " << environment["CNI_PATH"];
      LOG(INFO) << "CNI_IFNAME: " << environment["CNI_IFNAME"];

      if (command.compare("ADD") == 0) {
        LOG(INFO) << "Using network configuration '"
            << stringify(networkConfigJSON.get())
            << "' for container " << containerId;
      }

      Try<Subprocess> s = subprocess(
          plugin.get(),
          {plugin.get()},
          Subprocess::PATH(networkConfigFile.get()),
          Subprocess::PIPE(),
          Subprocess::PIPE(),
          nullptr,
          environment);

      if (s.isError()) {
        LOG(ERROR) << "Cannot invoke CNI plugin: " << s.error();
        continue;
      }

      return std::make_tuple(s.get(), networkName);
    }
    return ::Error("Could not match networkinfo to any CNI");
  }

  Try<std::string> attachCNISuccess(
    const Try<Subprocess> sub)
  {

    const Future<Option<int>>& subStat = sub->status();
    const Future<string>& output = io::read(sub->out().get());
    const Future<string>& error = io::read(sub->err().get());     

    if (!subStat.isReady()) {
      return ::Error("Failed to get the exit status of the CNI plugin subprocess.");
    }

    // CNI plugin will print result (in case of success) or error (in
    // case of failure) to stdout.
    if (!output.isReady()) {
      return ::Error("Failed to read stdout from the CNI plugin subprocess."); 
    }

    if (subStat.get() != 0) {
      if (!error.isReady()) {
        return ::Error("Failed to read stderr from the CNI plugin subprocess.");
      }

      return ::Error("The CNI plugin failed to attach container " + stringify(containerId) +
                     " to CNI network '" + "': stdout='" + output.get() + "', stderr='" 
                     + error.get() + "'");
    }

    Try<NetworkInfo> network = parseNetworkInfo(output.get());
    if (network.isError()) {
      return ::Error("Failed to parse the output of the CNI plugin:" + network.error());
    }

    LOG(INFO) << output.get();

    return network->ip_addresses(0).ip_address().c_str();
  }

  Try<NetworkInfo> parseNetworkInfo(const string& s) 
  {
    NetworkInfo ni;
    Try<JSON::Object> json = JSON::parse<JSON::Object>(s);
    if (json.isError()) {
      return ::Error("JSON parse CNI network info failed: " + json.error());
    }

    Result<JSON::Object> ip4 = json->at<JSON::Object>("ip4");
    if (ip4.isError()) {
      return ::Error("JSON parse CNI network info failed: " + ip4.error());
    }

    Result<JSON::String> ipAddress = ip4->at<JSON::String>("ip");
    if (ip4.isError()) {
      return ::Error("JSON parse CNI network info failed: " + ipAddress.error());
    }

    // remove netmask from the IP string
    std::size_t pos = ipAddress->value.find("/"); 
    if (pos != std::string::npos) {
        ipAddress->value.erase(pos);
    }

    ni.add_ip_addresses()->set_ip_address(ipAddress->value);

    return ni;
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

    vector<string> namespaces;
    if (healthCheck.type() == HealthCheck::HTTP ||
        healthCheck.type() == HealthCheck::TCP) {
      // Make sure HTTP and TCP health checks are run
      // from the container's network namespace.
      namespaces.push_back("net");
    }

    const checks::runtime::Docker dockerRuntime{
      namespaces,
      containerPid,
      docker->getPath(),
      docker->getSocket(),
      containerName
    };

    Try<Owned<checks::HealthChecker>> _checker =
      checks::HealthChecker::create(
          healthCheck,
          launcherDir,
          defer(self(), &Self::taskHealthUpdated, lambda::_1),
          task.task_id(),
          dockerRuntime);

    if (_checker.isError()) {
      // TODO(gilbert): Consider ABORT and return a TASK_FAILED here.
      LOG(ERROR) << "Failed to create health checker: "
                 << _checker.error();
    } else {
      checker = _checker.get();
    }
  }

  // TODO(alexr): Introduce a state enum and document transitions,
  // see MESOS-5252.
  bool killed;
  bool terminated;
  bool killedByHealthCheck;
  bool killedByTaskCompletionTimeout;

  Option<Timer> taskCompletionTimer;

  string launcherDir;
  Owned<Docker> docker;
  string containerName;
  string sandboxDirectory;
  string mappedDirectory;
  Duration shutdownGracePeriod;
  map<string, string> taskEnvironment;
  Option<ContainerDNSInfo> defaultContainerDNS;
  bool cgroupsEnableCfs;
  string network_cni_plugins_dir;
  string network_cni_config_dir;
  string containerId;
  mesos::ContainerInfo container;  

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


DockerExecutor::DockerExecutor(
    const Owned<Docker>& docker,
    const string& container,
    const string& sandboxDirectory,
    const string& mappedDirectory,
    const Duration& shutdownGracePeriod,
    const string& launcherDir,
    const map<string, string>& taskEnvironment,
    const Option<ContainerDNSInfo>& defaultContainerDNS,
    bool cgroupsEnableCfs,
    const string& network_cni_plugins_dir,
    const string& network_cni_config_dir)    
{
  process = Owned<DockerExecutorProcess>(new DockerExecutorProcess(
      docker,
      container,
      sandboxDirectory,
      mappedDirectory,
      shutdownGracePeriod,
      launcherDir,
      taskEnvironment,
      defaultContainerDNS,
      cgroupsEnableCfs,
      network_cni_plugins_dir,
      network_cni_config_dir));      

  spawn(process.get());
}


DockerExecutor::~DockerExecutor()
{
  terminate(process.get());
  wait(process.get());
}


void DockerExecutor::registered(
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


void DockerExecutor::reregistered(
    ExecutorDriver* driver,
    const SlaveInfo& slaveInfo)
{
  dispatch(process.get(),
           &DockerExecutorProcess::reregistered,
           driver,
           slaveInfo);
}


void DockerExecutor::disconnected(ExecutorDriver* driver)
{
  dispatch(process.get(), &DockerExecutorProcess::disconnected, driver);
}


void DockerExecutor::launchTask(ExecutorDriver* driver, const TaskInfo& task)
{
  dispatch(process.get(), &DockerExecutorProcess::launchTask, driver, task);
}


void DockerExecutor::killTask(ExecutorDriver* driver, const TaskID& taskId)
{
  // Need to disambiguate overloaded function.
  void (DockerExecutorProcess::*killTaskMethod)(
      ExecutorDriver*, const TaskID&, const Option<KillPolicy>&)
    = &DockerExecutorProcess::killTask;

  process::dispatch(process.get(), killTaskMethod, driver, taskId, None());
}


void DockerExecutor::frameworkMessage(
    ExecutorDriver* driver,
    const string& data)
{
  dispatch(process.get(),
           &DockerExecutorProcess::frameworkMessage,
           driver,
           data);
}


void DockerExecutor::shutdown(ExecutorDriver* driver)
{
  dispatch(process.get(), &DockerExecutorProcess::shutdown, driver);
}


void DockerExecutor::error(ExecutorDriver* driver, const string& data)
{
  dispatch(process.get(), &DockerExecutorProcess::error, driver, data);
}


void DockerExecutor::killTask(
    ExecutorDriver* driver,
    const TaskID& taskId,
    const Option<KillPolicy>& killPolicyOverride)
{
  // Need to disambiguate overloaded function.
  void (DockerExecutorProcess::*killTaskMethod)(
      ExecutorDriver*, const TaskID&, const Option<KillPolicy>&)
    = &DockerExecutorProcess::killTask;

  process::dispatch(
      process.get(),
      killTaskMethod,
      driver,
      taskId,
      killPolicyOverride);
}

} // namespace docker {
} // namespace internal {
} // namespace mesos {
