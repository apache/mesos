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

#ifdef MESOS_WEBUI

#include <process/process.hpp>

#include "slave/webui.hpp"

#include "common/option.hpp"
#include "common/utils.hpp"
#include "common/webui_utils.hpp"

#include "configurator/configuration.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace webui {

void start(const process::PID<Slave>& slave, const Configuration& conf)
{
  std::vector<std::string> args(4);
  args[0] = "--slave_port=" + utils::stringify(slave.port);
  args[1] = "--webui_port=" + conf.get("webui_port", "8081");
  args[2] = "--log_dir=" + conf.get("log_dir", FLAGS_log_dir);

  std::string workDir = "work";  // Default work directory.

  // Now look for configured work directory.
  Option<std::string> option = conf.get("work_dir");
  if (option.isNone()) {
    // Okay, then look for a home directory instead.
    option = conf.get("home");
    if (option.isSome()) {
      workDir = option.get() + "/work";
    }
  } else {
    workDir = option.get();
  }

  args[3] = "--work_dir=" + workDir;

  utils::webui::start(conf, "slave/webui.py", args);
}

} // namespace webui {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // MESOS_WEBUI
