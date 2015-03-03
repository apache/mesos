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

#include <string>

#include <mesos/hook.hpp>
#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/hook.hpp>

#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

using std::string;

using namespace mesos;

// Must be kept in sync with variables of the same name in
// tests/hook_tests.cpp.
const char* testLabelKey = "MESOS_Test_Label";
const char* testLabelValue = "ApacheMesos";
const char* testEnvironmentVariableName = "MESOS_TEST_ENVIRONMENT_VARIABLE";

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
    Label *label = labels.add_labels();
    label->set_key(testLabelKey);
    label->set_value(testLabelValue);

    return labels;
  }


  // In this hook, we create a temporary file and add its path to an
  // environment variable.  Later on, this environment variable is
  // looked up by the removeExecutorHook to locate and delete this
  // file.
  virtual Result<Environment> slaveLaunchExecutorEnvironmentDecorator(
      const ExecutorInfo& executorInfo,
      const TaskInfo& taskInfo)
  {
    LOG(INFO) << "Executing 'slaveLaunchExecutorEnvironmentDecorator' hook";

    // Find the label value for the label that was created in the
    // label decorator hook above.
    Option<string> labelValue;
    foreach (const Label& label, taskInfo.labels().labels()) {
      if (label.key() == testLabelKey) {
        labelValue = label.value();
        CHECK_EQ(labelValue.get(), testLabelValue);
      }
    }
    CHECK_SOME(labelValue);

    // Create a temporary file.
    Try<string> file = os::mktemp();
    CHECK_SOME(file);
    CHECK_SOME(os::write(file.get(), labelValue.get()));

    // Inject file path into command environment.
    Environment environment;
    Environment::Variable* variable = environment.add_variables();
    variable->set_name(testEnvironmentVariableName);
    variable->set_value(file.get());

    return environment;
  }


  // This hook locates the file created by environment decorator hook
  // and deletes it.
  virtual Try<Nothing> slaveRemoveExecutorHook(
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo)
  {
    LOG(INFO) << "Executing 'slaveRemoveExecutorHook'";

    foreach (const Environment::Variable& variable,
        executorInfo.command().environment().variables()) {
      if (variable.name() == testEnvironmentVariableName) {
        string path = variable.value();
        // The removeExecutor hook may be called multiple times; thus
        // we ignore the subsequent calls.
        if (os::stat::isfile(path)) {
          CHECK_SOME(os::rm(path));
        }
        break;
      }
    }
    return Nothing();
  }
};


static Hook* createHook(const Parameters& parameters)
{
  return new TestHook();
}


// Declares a Hook module named 'TestHook'.
mesos::modules::Module<Hook> org_apache_mesos_TestHook(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "Test Hook module.",
    NULL,
    createHook);
