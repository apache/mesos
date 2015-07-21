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

#include <mesos/hook.hpp>
#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/hook.hpp>

#include <process/future.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "messages/messages.hpp"

using namespace mesos;

using process::Future;

// Must be kept in sync with variables of the same name in
// tests/hook_tests.cpp.
const char* testLabelKey = "MESOS_Test_Label";
const char* testLabelValue = "ApacheMesos";
const char* testRemoveLabelKey = "MESOS_Test_Remove_Label";

class HookProcess : public ProtobufProcess<HookProcess>
{
public:
  Future<Nothing> signal()
  {
    internal::HookExecuted message;
    message.set_module("org_apache_mesos_TestHook");
    send(self(), message);
    return Nothing();
  }
};


class TestHook : public Hook
{
public:
  virtual Result<Labels> masterLaunchTaskLabelDecorator(
      const TaskInfo& taskInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo)
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

  // TODO(nnielsen): Split hook tests into multiple modules to avoid
  // interference.
  virtual Result<Labels> slaveRunTaskLabelDecorator(
      const TaskInfo& taskInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo)
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
  virtual Result<Environment> slaveExecutorEnvironmentDecorator(
      const ExecutorInfo& executorInfo)
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

  // This hook locates the file created by environment decorator hook
  // and deletes it.
  virtual Try<Nothing> slaveRemoveExecutorHook(
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo)
  {
    LOG(INFO) << "Executing 'slaveRemoveExecutorHook'";

    // Send a HookExecuted message to ourself. The hook test
    // "VerifySlaveLaunchExecutorHook" will wait for the testing
    // infrastructure to intercept this message. The intercepted message
    // indicates successful execution of this hook.
    HookProcess hookProcess;
    process::spawn(&hookProcess);
    process::dispatch(hookProcess, &HookProcess::signal).await();
    process::terminate(hookProcess);
    process::wait(hookProcess);

    return Nothing();
  }


  virtual Result<Labels> slaveTaskStatusLabelDecorator(
      const FrameworkID& frameworkId,
      const TaskStatus& status)
  {
    LOG(INFO) << "Executing 'slaveTaskStatusLabelDecorator' hook";

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

    return labels;
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
    NULL,
    createHook);
