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

#include <iostream>
#include <iomanip>
#include <list>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>

#include <mesos/type_utils.hpp>

#include <process/async.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/reap.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include "common/status_utils.hpp"

#include "slave/paths.hpp"

#include "slave/containerizer/external_containerizer.hpp"


using lambda::bind;

using std::list;
using std::map;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

using tuples::tuple;

using namespace process;

namespace mesos {
namespace internal {
namespace slave {

using state::ExecutorState;
using state::FrameworkState;
using state::RunState;
using state::SlaveState;


Try<ExternalContainerizer*> ExternalContainerizer::create(const Flags& flags)
{
  return new ExternalContainerizer(flags);
}


// Validate the invocation result.
static Option<Error> validate(
    const Future<Option<int>>& future)
{
  if (!future.isReady()) {
    return Error("Status not ready");
  }

  Option<int> status = future.get();
  if (status.isNone()) {
    return Error("External containerizer has no status available");
  }

  // The status is a waitpid-result which has to be checked for SIGNAL
  // based termination before masking out the exit-code.
  if (!WIFEXITED(status.get()) || WEXITSTATUS(status.get()) != 0) {
    return Error("Externel containerizer " + WSTRINGIFY(status.get()));
  }

  return None();
}


// Validate the invocation results and extract a piped protobuf
// message.
template<typename T>
static Try<T> result(
    const Future<tuple<Future<Result<T>>, Future<Option<int>>>>& future)
{
  if (!future.isReady()) {
    return Error("Could not receive any result");
  }

  Option<Error> error = validate(tuples::get<1>(future.get()));
  if (error.isSome()) {
    return error.get();
  }

  Future<Result<T>> result = tuples::get<0>(future.get());
  if (result.isFailed()) {
    return Error("Could not receive any result: " + result.failure());
  }

  if (result.get().isError()) {
    return Error("Could not receive any result: " + result.get().error());
  }

  if (result.get().isNone()) {
    return Error("Could not receive any result");
  }

  return result.get().get();
}


ExternalContainerizer::ExternalContainerizer(const Flags& flags)
  : process(new ExternalContainerizerProcess(flags))
{
  spawn(process.get());
}


ExternalContainerizer::~ExternalContainerizer()
{
  terminate(process.get());
  process::wait(process.get());
}


Future<Nothing> ExternalContainerizer::recover(
    const Option<state::SlaveState>& state)
{
  return dispatch(process.get(),
                  &ExternalContainerizerProcess::recover,
                  state);
}


Future<bool> ExternalContainerizer::launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
    return dispatch(process.get(),
                    &ExternalContainerizerProcess::launch,
                    containerId,
                    None(),
                    executorInfo,
                    directory,
                    user,
                    slaveId,
                    slavePid,
                    checkpoint);
}


Future<bool> ExternalContainerizer::launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
    return dispatch(process.get(),
                    &ExternalContainerizerProcess::launch,
                    containerId,
                    taskInfo,
                    executorInfo,
                    directory,
                    user,
                    slaveId,
                    slavePid,
                    checkpoint);
}


Future<Nothing> ExternalContainerizer::update(
    const ContainerID& containerId,
    const Resources& resources)
{
    return dispatch(process.get(),
                    &ExternalContainerizerProcess::update,
                    containerId,
                    resources);
}


Future<ResourceStatistics> ExternalContainerizer::usage(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &ExternalContainerizerProcess::usage,
                  containerId);
}


Future<containerizer::Termination> ExternalContainerizer::wait(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &ExternalContainerizerProcess::wait,
                  containerId);
}


void ExternalContainerizer::destroy(const ContainerID& containerId)
{
  dispatch(process.get(),
           &ExternalContainerizerProcess::destroy,
           containerId);
}


Future<hashset<ContainerID>> ExternalContainerizer::containers()
{
  return dispatch(process.get(),
                  &ExternalContainerizerProcess::containers);
}


ExternalContainerizerProcess::ExternalContainerizerProcess(
    const Flags& _flags) : flags(_flags) {}


Future<Nothing> ExternalContainerizerProcess::recover(
    const Option<state::SlaveState>& state)
{
  LOG(INFO) << "Recovering containerizer";

  // Ask the external containerizer to recover its internal state.
  Try<Subprocess> invoked = invoke("recover");

  if (invoked.isError()) {
    return Failure("Recover failed: " + invoked.error());
  }

  return invoked.get().status()
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::_recover,
        state,
        lambda::_1));
}


Future<Nothing> ExternalContainerizerProcess::_recover(
    const Option<state::SlaveState>& state,
    const Future<Option<int>>& future)
{
  VLOG(1) << "Recover validation callback triggered";

  Option<Error> error = validate(future);

  if (error.isSome()) {
    return Failure("Recover failed: " + error.get().message);
  }

  // Gather the active containers from the external containerizer.
  return containers()
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::__recover,
        state,
        lambda::_1));
}


Future<Nothing> ExternalContainerizerProcess::__recover(
    const Option<state::SlaveState>& state,
    const hashset<ContainerID>& containers)
{
  VLOG(1) << "Recover continuation triggered";

  // An orphaned container is known to the external containerizer but
  // not to the slave, thus not recoverable but pending.
  hashset<ContainerID> orphaned = containers;

  if (state.isSome()) {
    foreachvalue (const FrameworkState& framework, state.get().frameworks) {
      foreachvalue (const ExecutorState& executor, framework.executors) {
        if (executor.info.isNone()) {
          LOG(WARNING) << "Skipping recovery of executor '" << executor.id
                       << "' of framework " << framework.id
                       << " because its info could not be recovered";
          continue;
        }

        if (executor.latest.isNone()) {
          LOG(WARNING) << "Skipping recovery of executor '" << executor.id
                       << "' of framework " << framework.id
                       << " because its latest run could not be recovered";
          continue;
        }

        // We are only interested in the latest run of the executor!
        const ContainerID& containerId = executor.latest.get();
        Option<RunState> run = executor.runs.get(containerId);
        CHECK_SOME(run);

        if (run.get().completed) {
          VLOG(1) << "Skipping recovery of executor '" << executor.id
                  << "' of framework " << framework.id
                  << " because its latest run "
                  << containerId << " is completed";
          continue;
        }

        // Containers the external containerizer does not have
        // information on, should be skipped as their state is not
        // recoverable.
        if (!containers.contains(containerId)) {
          LOG(WARNING) << "Skipping recovery of executor '" << executor.id
                       << "' of framework " << framework.id
                       << " because the external containerizer has not "
                       << " identified " << containerId << " as active";
          continue;
        }

        LOG(INFO) << "Recovering container '" << containerId
                  << "' for executor '" << executor.id
                  << "' of framework " << framework.id;

        Option<string> user = None();
        if (flags.switch_user) {
          // The command (either in form of task or executor command)
          // can define a specific user to run as. If present, this
          // precedes the framework user value.
          if (executor.info.isSome() &&
              executor.info.get().command().has_user()) {
            user = executor.info.get().command().user();
          } else if (framework.info.isSome()) {
            user = framework.info.get().user();
          }
        }

        // Re-create the sandbox for this container.
        const string directory = paths::createExecutorDirectory(
            flags.work_dir,
            state.get().id,
            framework.id,
            executor.id,
            containerId,
            user);

        Sandbox sandbox(directory, user);

        // Collect this container as being active.
        actives.put(containerId, Owned<Container>(new Container(sandbox)));

        // Assume that this container had been launched, if this proves
        // to be wrong, the containerizer::Termination delivered by the
        // subsequent wait invocation will tell us.
        actives[containerId]->launched.set(Nothing());

        // Remove this container from the orphan collection as it is not
        // orphaned.
        orphaned.erase(containerId);
      }
    }
  }

  // Done when we got no orphans to take care of.
  if (orphaned.empty()) {
    VLOG(1) << "Recovery done";
    return Nothing();
  }

  list<Future<containerizer::Termination>> futures;

  // Enforce a 'destroy' on all orphaned containers.
  foreach (const ContainerID& containerId, orphaned) {
    LOG(INFO) << "Destroying container '" << containerId << "' as it "
              << "is in an orphaned state.";
    // For being able to wait on an orphan, we need to create an
    // internal Container state - we just can not have a sandbox for
    // it.
    actives.put(containerId, Owned<Container>(new Container(None())));
    actives[containerId]->launched.set(Nothing());

    // Wrap the orphan destruction by a wait so we know when it is
    // finally gone.
    futures.push_back(_wait(containerId));

    destroy(containerId);
  }

  VLOG(1) << "Awaiting all orphans to get destructed";

  // Orphan destruction needs to complete before we satisfy the
  // returned future.
  return collect(futures)
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::___recover));
}


Future<Nothing> ExternalContainerizerProcess::___recover()
{
  VLOG(1) << "Recovery done";
  return Nothing();
}


Future<bool> ExternalContainerizerProcess::launch(
    const ContainerID& containerId,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executor,
    const std::string& directory,
    const Option<std::string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  LOG(INFO) << "Launching container '" << containerId << "'";

  if (actives.contains(containerId)) {
    return Failure("Cannot start already running container '" +
                   containerId.value() + "'");
  }

  map<string, string> environment = executorEnvironment(
      executor,
      directory,
      slaveId,
      slavePid,
      checkpoint,
      flags.recovery_timeout);

  // TODO(tillt): Consider moving this into
  // Containerizer::executorEnvironment.
  if (!flags.hadoop_home.empty()) {
    environment["HADOOP_HOME"] = flags.hadoop_home;
  }

  if (flags.default_container_image.isSome()) {
    environment["MESOS_DEFAULT_CONTAINER_IMAGE"] =
      flags.default_container_image.get();
  }

  containerizer::Launch launch;
  launch.mutable_container_id()->CopyFrom(containerId);
  if (taskInfo.isSome()) {
    launch.mutable_task_info()->CopyFrom(taskInfo.get());
  }
  launch.mutable_executor_info()->CopyFrom(executor);
  launch.set_directory(directory);
  if (user.isSome()) {
    launch.set_user(user.get());
  }
  launch.mutable_slave_id()->CopyFrom(slaveId);
  launch.set_slave_pid(slavePid);
  launch.set_checkpoint(checkpoint);

  Sandbox sandbox(directory, user);

  Try<Subprocess> invoked = invoke(
      "launch",
      launch,
      sandbox,
      environment);

  if (invoked.isError()) {
    return Failure("Launch of container '" + containerId.value() +
                   "' failed: " + invoked.error());
  }

  // Checkpoint the executor's pid if requested.
  // NOTE: Containerizer(s) currently rely on their state being
  // persisted in the slave. However, that responsibility should have
  // been delegated to the containerizer.
  // To work around the mandatory forked pid recovery, we need to
  // checkpoint one. See MESOS-1328 and MESOS-923.
  // TODO(tillt): Remove this entirely as soon as MESOS-923 is fixed.
  if (checkpoint) {
    const string& path = slave::paths::getForkedPidPath(
        slave::paths::getMetaRootDir(flags.work_dir),
        slaveId,
        executor.framework_id(),
        executor.executor_id(),
        containerId);

    LOG(INFO) << "Checkpointing executor's forked pid " << invoked.get().pid()
              << " to '" << path <<  "'";

    Try<Nothing> checkpointed =
      slave::state::checkpoint(path, stringify(invoked.get().pid()));

    if (checkpointed.isError()) {
      LOG(ERROR) << "Failed to checkpoint executor's forked pid to '"
                 << path << "': " << checkpointed.error();

      return Failure("Could not checkpoint executor's pid");
    }
  }

  // Record the container launch intend.
  actives.put(containerId, Owned<Container>(new Container(sandbox)));

  return invoked.get().status()
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::_launch,
        containerId,
        lambda::_1))
    .onAny(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::__launch,
        containerId,
        lambda::_1));
}


Future<bool> ExternalContainerizerProcess::_launch(
    const ContainerID& containerId,
    const Future<Option<int>>& future)
{
  VLOG(1) << "Launch validation callback triggered on container '"
          << containerId << "'";

  Option<Error> error = validate(future);
  if (error.isSome()) {
    return Failure("Could not launch container '" +
                   containerId.value() + "': " + error.get().message);
  }

  VLOG(1) << "Launch finishing up for container '" << containerId << "'";

  // Launch is done, we can now process all other commands that might
  // have gotten chained up.
  actives[containerId]->launched.set(Nothing());

  return true;
}


void ExternalContainerizerProcess::__launch(
    const ContainerID& containerId,
    const Future<bool>& future)
{
  VLOG(1) << "Launch confirmation callback triggered on container '"
          << containerId << "'";

  // We need to cleanup whenever this callback was invoked due to a
  // failure or discarded future.
  if (!future.isReady()) {
    cleanup(containerId);
  }
}


Future<containerizer::Termination> ExternalContainerizerProcess::wait(
    const ContainerID& containerId)
{
  VLOG(1) << "Wait triggered on container '" << containerId << "'";

  if (!actives.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "' not running");
  }

  // Defer wait until launch is done.
  return actives[containerId]->launched.future()
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::_wait,
        containerId));
}


Future<containerizer::Termination> ExternalContainerizerProcess::_wait(
    const ContainerID& containerId)
{
  VLOG(1) << "Wait continuation triggered on container '"
          << containerId << "'";

  if (!actives.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "' not running");
  }

  // We must not run multiple 'wait' invocations concurrently on the
  // same container.
  if (actives[containerId]->pid.isSome()) {
    VLOG(2) << "Already waiting for " << containerId;
    return actives[containerId]->termination.future();
  }

  containerizer::Wait wait;
  wait.mutable_container_id()->CopyFrom(containerId);

  Try<Subprocess> invoked = invoke(
      "wait",
      wait,
      actives[containerId]->sandbox);

  if (invoked.isError()) {
    // 'wait' has failed, we need to tear down everything now.
    unwait(containerId);
    return Failure("Wait on container '" + containerId.value() +
                   "' failed: " + invoked.error());
  }

  actives[containerId]->pid = invoked.get().pid();

  // Invoke the protobuf::read asynchronously.
  // TODO(tillt): Consider moving protobuf::read into libprocess and
  // making it work fully asynchronously.
  Result<containerizer::Termination>(*read)(int, bool, bool) =
    &::protobuf::read<containerizer::Termination>;

  Future<Result<containerizer::Termination>> future = async(
      read, invoked.get().out().get(), false, false);

  // Await both, a protobuf Message from the subprocess as well as
  // its exit.
  await(future, invoked.get().status())
    .onAny(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::__wait,
        containerId,
        lambda::_1));

  return actives[containerId]->termination.future();
}


void ExternalContainerizerProcess::__wait(
    const ContainerID& containerId,
    const Future<tuple<
        Future<Result<containerizer::Termination>>,
        Future<Option<int>>>>& future)
{
  VLOG(1) << "Wait callback triggered on container '" << containerId << "'";

  if (!actives.contains(containerId)) {
    LOG(ERROR) << "Container '" << containerId << "' not running";
    return;
  }

  // When 'wait' was terminated by 'destroy', it is getting SIGKILLed
  // (see unwait). We need to test for that specific case as otherwise
  // the result validation below will return an error due to a non 0
  // exit status.
  if (actives[containerId]->destroying && future.isReady()) {
    Future<Option<int>> statusFuture = tuples::get<1>(future.get());
    if (statusFuture.isReady()) {
      Option<int> status = statusFuture.get();
      if (status.isSome()) {
        VLOG(2) << "Wait got destroyed on '" << containerId << "'";
        containerizer::Termination termination;
        // 'killed' must only be true when a resource limitation
        // had to be enforced through terminating a task.
        // TODO(tillt): Consider renaming 'killed' towards 'limited'.
        termination.set_killed(false);
        termination.set_message("");
        termination.set_status(status.get());
        actives[containerId]->termination.set(termination);
        cleanup(containerId);
        return;
      }
    }
  }

  Try<containerizer::Termination> termination =
    result<containerizer::Termination>(future);

  if (termination.isError()) {
    VLOG(2) << "Wait termination failed on '" << containerId << "'";
    // 'wait' has failed, we need to tear down everything now.
    actives[containerId]->termination.fail(termination.error());
    unwait(containerId);
  } else {
    VLOG(2) << "Wait Termination: " << termination.get().DebugString();
    // Set the promise to alert others waiting on this container.
    actives[containerId]->termination.set(termination.get());
  }

  // The container has been waited on, we can safely cleanup now.
  cleanup(containerId);
}


Future<Nothing> ExternalContainerizerProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  VLOG(1) << "Update triggered on container '" << containerId << "'";

  if (!actives.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "'' not running");
  }

  // Defer update until launch is done.
  return actives[containerId]->launched.future()
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::_update,
        containerId,
        resources));
}


Future<Nothing> ExternalContainerizerProcess::_update(
    const ContainerID& containerId,
    const Resources& resources)
{
  VLOG(1) << "Update continuation triggered on container '"
          << containerId << "'";

  if (!actives.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "'' not running");
  }

  actives[containerId]->resources = resources;

  containerizer::Update update;
  update.mutable_container_id()->CopyFrom(containerId);
  update.mutable_resources()->CopyFrom(resources);

  Try<Subprocess> invoked = invoke(
      "update",
      update,
      actives[containerId]->sandbox);

  if (invoked.isError()) {
    return Failure("Update of container '" + containerId.value() +
                   "' failed: " + invoked.error());
  }

  return invoked.get().status()
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::__update,
        containerId,
        lambda::_1));
}


Future<Nothing> ExternalContainerizerProcess::__update(
    const ContainerID& containerId,
    const Future<Option<int>>& future)
{
  VLOG(1) << "Update callback triggered on container '" << containerId << "'";

  if (!actives.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "' not running");
  }

  Option<Error> error = validate(future);
  if (error.isSome()) {
    return Failure(error.get());
  }

  return Nothing();
}


Future<ResourceStatistics> ExternalContainerizerProcess::usage(
    const ContainerID& containerId)
{
  VLOG(1) << "Usage triggered on container '" << containerId << "'";

  if (!actives.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "'' not running");
  }

  // Defer usage until launch is done.
  return actives[containerId]->launched.future()
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::_usage,
        containerId));
}


Future<ResourceStatistics> ExternalContainerizerProcess::_usage(
    const ContainerID& containerId)
{
  VLOG(1) << "Usage continuation on container '" << containerId << "'";

  if (!actives.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "'' not running");
  }

  containerizer::Usage usage;
  usage.mutable_container_id()->CopyFrom(containerId);

  Try<Subprocess> invoked = invoke(
      "usage",
      usage,
      actives[containerId]->sandbox);

  if (invoked.isError()) {
    // 'usage' has failed but we keep the container alive for now.
    return Failure("Usage on container '" + containerId.value() +
                   "' failed: " + invoked.error());
  }

  Result<ResourceStatistics>(*read)(int, bool, bool) =
    &::protobuf::read<ResourceStatistics>;

  Future<Result<ResourceStatistics>> future = async(
      read, invoked.get().out().get(), false, false);

  // Await both, a protobuf Message from the subprocess as well as
  // its exit.
  return await(future, invoked.get().status())
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::__usage,
        containerId,
        lambda::_1));
}


Future<ResourceStatistics> ExternalContainerizerProcess::__usage(
    const ContainerID& containerId,
    const Future<tuple<
        Future<Result<ResourceStatistics>>,
        Future<Option<int>>>>& future)
{
  VLOG(1) << "Usage callback triggered on container '" << containerId << "'";

  if (!actives.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "' not running");
  }

  Try<ResourceStatistics> statistics = result<ResourceStatistics>(future);

  if (statistics.isError()) {
    return Failure(statistics.error());
  }

  VLOG(2) << "Container '" << containerId << "' "
          << "total mem usage "
          << statistics.get().mem_rss_bytes() << " "
          << "total CPU user usage "
          << statistics.get().cpus_user_time_secs() << " "
          << "total CPU system usage "
          << statistics.get().cpus_system_time_secs();

  return statistics.get();
}


void ExternalContainerizerProcess::destroy(const ContainerID& containerId)
{
  VLOG(1) << "Destroy triggered on container '" << containerId << "'";

  if (!actives.contains(containerId)) {
    LOG(ERROR) << "Container '" << containerId << "' not running";
    return;
  }

  // Defer destroy until launch is done.
  actives[containerId]->launched.future()
    .onAny(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::_destroy,
        containerId));
}


void ExternalContainerizerProcess::_destroy(const ContainerID& containerId)
{
  VLOG(1) << "Destroy continuation on container '" << containerId << "'";

  if (!actives.contains(containerId)) {
    LOG(ERROR) << "Container '" << containerId << "' not running";
    return;
  }

  if (actives[containerId]->destroying) {
    LOG(WARNING) << "Container '" << containerId
                 << "' is already being destroyed";
    return;
  }
  actives[containerId]->destroying = true;

  containerizer::Destroy destroy;
  destroy.mutable_container_id()->CopyFrom(containerId);

  Try<Subprocess> invoked = invoke(
      "destroy",
      destroy,
      actives[containerId]->sandbox);

  if (invoked.isError()) {
    LOG(ERROR) << "Destroy of container '" << containerId
               << "' failed: " << invoked.error();
    unwait(containerId);
    return;
  }

  invoked.get().status()
    .onAny(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::__destroy,
        containerId,
        lambda::_1));
}


void ExternalContainerizerProcess::__destroy(
    const ContainerID& containerId,
    const Future<Option<int>>& future)
{
  VLOG(1) << "Destroy callback triggered on container '" << containerId << "'";

  if (!actives.contains(containerId)) {
    LOG(ERROR) << "Container '" << containerId << "' not running ";
    return;
  }

  Option<Error> error = validate(future);
  if (error.isSome()) {
    LOG(ERROR) << "Destroy of container '" << containerId
               << "' failed: " << error.get().message;
  }

  // Additionally to the optional external destroy-command, we need to
  // terminate the external containerizer's "wait" process.
  unwait(containerId);
}


Future<hashset<ContainerID>> ExternalContainerizerProcess::containers()
{
  VLOG(1) << "Containers triggered";

  Try<Subprocess> invoked = invoke("containers");

  if (invoked.isError()) {
    return Failure("Containers failed: " + invoked.error());
  }

  Result<containerizer::Containers>(*read)(int, bool, bool) =
    &::protobuf::read<containerizer::Containers>;

  Future<Result<containerizer::Containers>> future = async(
      read, invoked.get().out().get(), false, false);

  // Await both, a protobuf Message from the subprocess as well as
  // its exit.
  return await(future, invoked.get().status())
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::_containers,
        lambda::_1));
}


Future<hashset<ContainerID>> ExternalContainerizerProcess::_containers(
    const Future<tuple<
        Future<Result<containerizer::Containers>>,
        Future<Option<int>>>>& future)
{
  VLOG(1) << "Containers callback triggered";

  Try<containerizer::Containers> containers =
    result<containerizer::Containers>(future);

  if (containers.isError()) {
    return Failure(containers.error());
  }

  hashset<ContainerID> result;
  foreach (const ContainerID& containerId, containers.get().containers()) {
    result.insert(containerId);
  }

  return result;
}


void ExternalContainerizerProcess::cleanup(const ContainerID& containerId)
{
  VLOG(1) << "Callback performing final cleanup of running state";

  if (actives.contains(containerId)) {
    actives.erase(containerId);
  } else {
    LOG(WARNING) << "Container '" << containerId << "' not running anymore";
  }
}


void ExternalContainerizerProcess::unwait(const ContainerID& containerId)
{
  if (!actives.contains(containerId)) {
    LOG(WARNING) << "Container '" << containerId << "' not running";
    return;
  }

  Option<pid_t> pid = actives[containerId]->pid;

  // Containers that are being waited on have the "wait" command's
  // pid assigned.
  if (pid.isNone()) {
    // If we reached this, launch most likely failed due to some error
    // on the external containerizer's side (e.g. returned non zero on
    // launch).
    LOG(WARNING) << "Container '" << containerId << "' not being waited on";
    cleanup(containerId);
    return;
  }

  // Terminate the containerizer.
  VLOG(2) << "About to send a SIGKILL to containerizer pid: " << pid.get();

  // TODO(tillt): Add graceful termination as soon as we have an
  // accepted way to do that in place.
  Try<list<os::ProcessTree>> trees =
    os::killtree(pid.get(), SIGKILL, true, true);

  if (trees.isError()) {
    LOG(WARNING) << "Failed to kill the process tree rooted at pid "
                 << pid.get() << ": " << trees.error();
    cleanup(containerId);
    return;
  }

  LOG(INFO) << "Killed the following process tree/s:\n"
            << stringify(trees.get());

  // The cleanup function will get invoked via __wait which triggers
  // once the external containerizer's "wait" subprocess gets
  // terminated.
}


// Post fork, pre exec function.
// TODO(tillt): Consider having the kernel notify us when our parent
// process dies e.g. by invoking prctl(PR_SET_PDEATHSIG, ..) on linux.
static int setup(const string& directory)
{
  // Put child into its own process session to prevent slave suicide
  // on child process SIGKILL/SIGTERM.
  if (::setsid() == -1) {
    return errno;
  }

  // Re/establish the sandbox conditions for the containerizer.
  if (!directory.empty()) {
    if (::chdir(directory.c_str()) == -1) {
      return errno;
    }
  }

  // Sync parent and child process.
  int sync = 0;
  while (::write(STDOUT_FILENO, &sync, sizeof(sync)) == -1 &&
         errno == EINTR);

  return 0;
}


Try<Subprocess> ExternalContainerizerProcess::invoke(
    const string& command,
    const Option<Sandbox>& sandbox,
    const Option<map<string, string>>& commandEnvironment)
{
  CHECK_SOME(flags.containerizer_path) << "containerizer_path not set";

  VLOG(1) << "Invoking external containerizer for method '" << command << "'";

  // Prepare a default environment.
  map<string, string> environment;
  environment["MESOS_LIBEXEC_DIRECTORY"] = flags.launcher_dir;
  environment["MESOS_WORK_DIRECTORY"] = flags.work_dir;

  // Update default environment with command specific one.
  if (commandEnvironment.isSome()) {
    environment.insert(
        commandEnvironment.get().begin(),
        commandEnvironment.get().end());
  }

  // Construct the command to execute.
  string execute = flags.containerizer_path.get() + " " + command;

  VLOG(2) << "calling: [" << execute << "]";
  VLOG_IF(2, sandbox.isSome()) << "directory: " << sandbox.get().directory;
  VLOG_IF(2, sandbox.isSome() &&
      sandbox.get().user.isSome()) << "user: " << sandbox.get().user.get();

  // Re/establish the sandbox conditions for the containerizer.
  if (sandbox.isSome() && sandbox.get().user.isSome()) {
    Try<Nothing> chown = os::chown(
        sandbox.get().user.get(),
        sandbox.get().directory);
    if (chown.isError()) {
      return Error("Failed to chown work directory: " + chown.error());
    }
  }

  // Fork exec of external process. Run a chdir and a setsid within
  // the child-context.
  Try<Subprocess> external = process::subprocess(
      execute,
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      environment,
      lambda::bind(&setup, sandbox.isSome() ? sandbox.get().directory
                                            : string()));

  if (external.isError()) {
    return Error("Failed to execute external containerizer: " +
                 external.error());
  }

  // Sync parent and child process to make sure we have done the
  // setsid within the child context before continuing.
  int sync;
  while (::read(external.get().out().get(), &sync, sizeof(sync)) == -1 &&
         errno == EINTR);

  // Set stderr into non-blocking mode.
  Try<Nothing> nonblock = os::nonblock(external.get().err().get());
  if (nonblock.isError()) {
    return Error("Failed to accept nonblock: " + nonblock.error());
  }

  // We are not setting stdin or stdout into non-blocking mode as
  // protobuf::read / write do currently not support it.

  // Redirect output (stderr) from the external containerizer to log
  // file in the executor work directory, chown'ing it if a user is
  // specified. When no sandbox is given, redirect to /dev/null to
  // prevent blocking on the subprocess side.
  // TODO(tillt): Consider switching to atomic close-on-exec instead.
  Try<int> err = os::open(
      sandbox.isSome() ? path::join(sandbox.get().directory, "stderr")
                       : "/dev/null",
      O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (err.isError()) {
    return Error(
        "Failed to redirect stderr: Failed to open: " +
        err.error());
  }

  if (sandbox.isSome() && sandbox.get().user.isSome()) {
    Try<Nothing> chown = os::chown(
        sandbox.get().user.get(),
        path::join(sandbox.get().directory, "stderr"));
    if (chown.isError()) {
      os::close(err.get());
      return Error(
          "Failed to redirect stderr: Failed to chown: " +
          chown.error());
    }
  }

  // TODO(tillt): Consider adding an overload to io::redirect
  // that accepts a file path as 'to' for further reducing code.
  io::redirect(external.get().err().get(), err.get());

  // Redirect does 'dup' the file descriptor, hence we can close the
  // original now.
  os::close(err.get());

  VLOG(2) << "Subprocess pid: " << external.get().pid() << ", "
          << "output pipe: " << external.get().out().get();

  return external;
}


Try<Subprocess> ExternalContainerizerProcess::invoke(
    const string& command,
    const google::protobuf::Message& message,
    const Option<Sandbox>& sandbox,
    const Option<map<string, string>>& commandEnvironment)
{
  Try<Subprocess> external = invoke(command, sandbox, commandEnvironment);
  if (external.isError()) {
    return external;
  }

  // Transmit protobuf data via stdout towards the external
  // containerizer. Each message is prefixed by its total size.
  Try<Nothing> write = ::protobuf::write(external.get().in().get(), message);
  if (write.isError()) {
    return Error("Failed to write protobuf to pipe: " + write.error());
  }

  return external;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
