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

#include <mesos/hook.hpp>
#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/hook.hpp>

#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "messages/messages.hpp"

using namespace mesos;

using std::map;
using std::string;

using process::Failure;
using process::Future;

// Must be kept in sync with variables of the same name in
// tests/hook_tests.cpp.
const char* testLabelKey = "MESOS_Test_Label";
const char* testLabelValue = "ApacheMesos";
const char* testRemoveLabelKey = "MESOS_Test_Remove_Label";
const char* testErrorLabelKey = "MESOS_Test_Error_Label";

class HookProcess : public ProtobufProcess<HookProcess>
{
public:
  HookProcess() : ProcessBase(process::ID::generate("example-hook")) {}

  void initialize() override
  {
    install<internal::HookExecuted>(
        &HookProcess::handler,
        &internal::HookExecuted::module);
  }

  void signal()
  {
    LOG(INFO) << "HookProcess emitting signal";

    internal::HookExecuted message;
    message.set_module("org_apache_mesos_TestHook");
    send(self(), message);
  }

  void handler(const process::UPID& from, const string& module)
  {
    LOG(INFO) << "HookProcess caught signal: " << module;

    promise.set(Nothing());
  }

  Future<Nothing> await()
  {
    return promise.future();
  }

private:
  process::Promise<Nothing> promise;
};


class TestHook : public Hook
{
public:
  Result<Labels> masterLaunchTaskLabelDecorator(
      const TaskInfo& taskInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo) override
  {
    LOG(INFO) << "Executing 'masterLaunchTaskLabelDecorator' hook";

    Labels labels;

    // Set one known label.
    Label* newLabel = labels.add_labels();
    newLabel->set_key(testLabelKey);
    newLabel->set_value(testLabelValue);

    // Remove the 'testRemoveLabelKey' label which was set by the test.
    foreach (const Label& oldLabel, taskInfo.labels().labels()) {
      if (oldLabel.key() != testRemoveLabelKey) {
        labels.add_labels()->CopyFrom(oldLabel);
      }
    }

    return labels;
  }

  Result<Resources> masterLaunchTaskResourceDecorator(
      const TaskInfo& taskInfo,
      const Resources& slaveResources) override
  {
    LOG(INFO) << "Executing 'masterLaunchTaskResourceDecorator' hook";

    // If slave does not declare network bandwidth resource,
    // don't set a default value for it.
    if (slaveResources.names().count("network_bandwidth") == 0) {
      return taskInfo.resources();
    }

    Resources taskResources = taskInfo.resources();

    // Add a default value for network bandwidth if absent.
    if (taskResources.names().count("network_bandwidth") == 0) {
      Resources bandwidth =
        CHECK_NOTERROR(Resources::parse("network_bandwidth:10"));

      CHECK(!taskResources.allocations().empty());
      bandwidth.allocate(taskResources.allocations().begin()->first);

      taskResources += bandwidth;
    }

    return taskResources;
  }

  Try<Nothing> masterSlaveLostHook(const SlaveInfo& slaveInfo) override
  {
    LOG(INFO) << "Executing 'masterSlaveLostHook' in agent '"
              << slaveInfo.id() << "'";

    // TODO(nnielsen): Add argument to signal(), so we can filter messages from
    // the `masterSlaveLostHook` from `slaveRemoveExecutorHook`.
    // NOTE: Will not be a problem **as long as** the test doesn't start any
    // tasks.
    HookProcess hookProcess;
    process::spawn(&hookProcess);
    Future<Nothing> future =
      process::dispatch(hookProcess, &HookProcess::await);

    process::dispatch(hookProcess, &HookProcess::signal);

    // Make sure we don't terminate the process before the message self-send has
    // completed.
    future.await();

    process::terminate(hookProcess);
    process::wait(hookProcess);

    return Nothing();
  }


  // TODO(nnielsen): Split hook tests into multiple modules to avoid
  // interference.
  Result<Labels> slaveRunTaskLabelDecorator(
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo) override
  {
    LOG(INFO) << "Executing 'slaveRunTaskLabelDecorator' hook";

    Labels labels;

    // Set one known label.
    Label* newLabel = labels.add_labels();
    newLabel->set_key("baz");
    newLabel->set_value("qux");

    // Remove label which was set by test.
    foreach (const Label& oldLabel, taskInfo.labels().labels()) {
      if (oldLabel.key() != "foo") {
        labels.add_labels()->CopyFrom(oldLabel);
      }
    }

    return labels;
  }


  // In this hook, we create a new environment variable "FOO" and set
  // it's value to "bar".
  Result<Environment> slaveExecutorEnvironmentDecorator(
      const ExecutorInfo& executorInfo) override
  {
    LOG(INFO) << "Executing 'slaveExecutorEnvironmentDecorator' hook";

    Environment environment;

    if (executorInfo.command().has_environment()) {
      environment.CopyFrom(executorInfo.command().environment());
    }

    Environment::Variable* variable = environment.add_variables();
    variable->set_name("FOO");
    variable->set_value("bar");

    return environment;
  }


  // In this hook, we check for the presence of a label, and if set
  // we return a failure, effectively failing the container creation.
  // Otherwise we add an environment variable to the executor and task.
  // Additionally, this hook creates a file named "foo" in the container
  // work directory (sandbox).
  Future<Option<DockerTaskExecutorPrepareInfo>>
    slavePreLaunchDockerTaskExecutorDecorator(
        const Option<TaskInfo>& taskInfo,
        const ExecutorInfo& executorInfo,
        const string& containerName,
        const string& containerWorkDirectory,
        const string& mappedSandboxDirectory,
        const Option<map<string, string>>& env) override
  {
    LOG(INFO) << "Executing 'slavePreLaunchDockerTaskExecutorDecorator' hook";

    if (taskInfo.isSome()) {
      foreach (const Label& label, taskInfo->labels().labels()) {
        if (label.key() == testErrorLabelKey) {
          return Failure("Spotted error label");
        }
      }
    }

    DockerTaskExecutorPrepareInfo prepareInfo;

    Environment* taskEnvironment =
      prepareInfo.mutable_taskenvironment();
    Environment::Variable* variable = taskEnvironment->add_variables();
    variable->set_name("HOOKTEST_TASK");
    variable->set_value("foo");

    Environment* executorEnvironment =
      prepareInfo.mutable_executorenvironment();
    variable = executorEnvironment->add_variables();
    variable->set_name("HOOKTEST_EXECUTOR");
    variable->set_value("bar");

    os::touch(path::join(containerWorkDirectory, "foo"));

    return prepareInfo;
  }


  Try<Nothing> slavePostFetchHook(
      const ContainerID& containerId,
      const string& directory) override
  {
    LOG(INFO) << "Executing 'slavePostFetchHook'";

    const string path = path::join(directory, "post_fetch_hook");

    if (os::exists(path)) {
      return os::rm(path);
    } else {
      return Nothing();
    }
  }


  // This hook locates the file created by environment decorator hook
  // and deletes it.
  Try<Nothing> slaveRemoveExecutorHook(
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo) override
  {
    LOG(INFO) << "Executing 'slaveRemoveExecutorHook'";

    // Send a HookExecuted message to ourself. The hook test
    // "VerifySlaveLaunchExecutorHook" will wait for the testing
    // infrastructure to intercept this message. The intercepted message
    // indicates successful execution of this hook.
    HookProcess hookProcess;
    process::spawn(&hookProcess);
    Future<Nothing> future =
      process::dispatch(hookProcess, &HookProcess::await);

    process::dispatch(hookProcess, &HookProcess::signal);

    // Make sure we don't terminate the process before the message self-send has
    // completed.
    future.await();

    process::terminate(hookProcess);
    process::wait(hookProcess);

    return Nothing();
  }


  Result<TaskStatus> slaveTaskStatusDecorator(
      const FrameworkID& frameworkId,
      const TaskStatus& status) override
  {
    LOG(INFO) << "Executing 'slaveTaskStatusDecorator' hook";

    Labels labels;

    // Set one known label.
    Label* newLabel = labels.add_labels();
    newLabel->set_key("bar");
    newLabel->set_value("qux");

    // Remove label which was set by test.
    foreach (const Label& oldLabel, status.labels().labels()) {
      if (oldLabel.key() != "foo") {
        labels.add_labels()->CopyFrom(oldLabel);
      }
    }

    TaskStatus result;
    result.mutable_labels()->CopyFrom(labels);

    // Set an IP address, a network isolation group, and a known label
    // in network info. This data is later validated by the
    // 'HookTest.VerifySlaveTaskStatusDecorator' test.
    NetworkInfo* networkInfo =
      result.mutable_container_status()->add_network_infos();

    NetworkInfo::IPAddress* ipAddress = networkInfo->add_ip_addresses();
    ipAddress->set_ip_address("4.3.2.1");
    networkInfo->add_groups("public");

    Label* networkInfoLabel = networkInfo->mutable_labels()->add_labels();
    networkInfoLabel->set_key("net_foo");
    networkInfoLabel->set_value("net_bar");

    return result;
  }


  Result<Resources> slaveResourcesDecorator(
      const SlaveInfo& slaveInfo) override
  {
    LOG(INFO) << "Executing 'slaveResourcesDecorator' hook";

    Resources resources;
    // Remove the existing "cpus" resource, it will be overwritten by the
    // current hook. Keep other resources unchanged.
    foreach (const Resource& resource, slaveInfo.resources()) {
      if (resource.name() != "cpus") {
        resources += resource;
      }
    }

    // Force the value of "cpus" to 4 and add a new custom resource named "foo"
    // of type set.
    resources += Resources::parse("cpus:4;foo:{bar,baz}").get();

    return resources;
  }


  Result<Attributes> slaveAttributesDecorator(
      const SlaveInfo& slaveInfo) override
  {
    LOG(INFO) << "Executing 'slaveAttributesDecorator' hook";

    Attributes attributes = slaveInfo.attributes();
    attributes.add(Attributes::parse("rack", "rack1"));

    return attributes;
  }
};


static Hook* createHook(const Parameters& parameters)
{
  return new TestHook();
}


// Declares a Hook module named 'org_apache_mesos_TestHook'.
mesos::modules::Module<Hook> org_apache_mesos_TestHook(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "Test Hook module.",
    nullptr,
    createHook);
