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

#include <mesos/mesos.hpp>

#include "common/utils.hpp"

#include "launcher/launcher.hpp"

using namespace mesos;
using namespace mesos::internal; // For 'utils'.


int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  FrameworkID frameworkId;
  frameworkId.set_value(utils::os::getenv("MESOS_FRAMEWORK_ID"));

  ExecutorID executorId;
  executorId.set_value(utils::os::getenv("MESOS_EXECUTOR_ID"));

  return mesos::internal::launcher::ExecutorLauncher(
      frameworkId,
      executorId,
      utils::os::getenv("MESOS_EXECUTOR_URI"),
      utils::os::getenv("MESOS_COMMAND"),
      utils::os::getenv("MESOS_USER"),
      utils::os::getenv("MESOS_WORK_DIRECTORY"),
      utils::os::getenv("MESOS_SLAVE_PID"),
      utils::os::getenv("MESOS_FRAMEWORKS_HOME", false),
      utils::os::getenv("MESOS_HADOOP_HOME"),
      utils::os::getenv("MESOS_REDIRECT_IO") == "1",
      utils::os::getenv("MESOS_SWITCH_USER") == "1",
      utils::os::getenv("MESOS_CONTAINER", false),
      Environment()).run();
}
