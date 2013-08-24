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

#include <mesos/mesos.hpp>

#include <stout/duration.hpp>
#include <stout/strings.hpp>
#include <stout/os.hpp>

#include "launcher/launcher.hpp"

using namespace mesos;
using namespace mesos::internal; // For 'utils'.

using std::string;


int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  SlaveID slaveId;
  slaveId.set_value(os::getenv("MESOS_SLAVE_ID"));

  FrameworkID frameworkId;
  frameworkId.set_value(os::getenv("MESOS_FRAMEWORK_ID"));

  ExecutorID executorId;
  executorId.set_value(os::getenv("MESOS_EXECUTOR_ID"));

  CommandInfo commandInfo;
  commandInfo.set_value(os::getenv("MESOS_COMMAND"));

  // Construct URIs from the encoded environment string.
  const std::string& uris = os::getenv("MESOS_EXECUTOR_URIS");
  foreach (const std::string& token, strings::tokenize(uris, " ")) {
    size_t pos = token.rfind("+"); // Delim between uri and exec permission.
    CHECK(pos != std::string::npos) << "Invalid executor uri token in env "
                                    << token;

    CommandInfo::URI uri;
    uri.set_value(token.substr(0, pos));
    uri.set_executable(token.substr(pos + 1) == "1");

    commandInfo.add_uris()->MergeFrom(uri);
  }

  bool checkpoint = os::getenv("MESOS_CHECKPOINT", false) == "1";

  Duration recoveryTimeout = slave::RECOVERY_TIMEOUT;

  // Get the recovery timeout if checkpointing is enabled.
  if (checkpoint) {
    string value = os::getenv("MESOS_RECOVERY_TIMEOUT", false);

    if (!value.empty()) {
      Try<Duration> _recoveryTimeout = Duration::parse(value);

      CHECK_SOME(_recoveryTimeout)
        << "Cannot parse MESOS_RECOVERY_TIMEOUT '" + value + "'";

      recoveryTimeout = _recoveryTimeout.get();
    }
  }

  return mesos::internal::launcher::ExecutorLauncher(
      slaveId,
      frameworkId,
      executorId,
      UUID::fromString(os::getenv("MESOS_EXECUTOR_UUID")),
      commandInfo,
      os::getenv("MESOS_USER"),
      os::getenv("MESOS_DIRECTORY"),
      os::getenv("MESOS_SLAVE_DIRECTORY"),
      os::getenv("MESOS_SLAVE_PID"),
      os::getenv("MESOS_FRAMEWORKS_HOME", false),
      os::getenv("MESOS_HADOOP_HOME"),
      os::getenv("MESOS_REDIRECT_IO") == "1",
      os::getenv("MESOS_SWITCH_USER") == "1",
      checkpoint,
      recoveryTimeout)
    .run();
}
