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

#include <process/process.hpp>

#include "master/webui.hpp"

#include "common/utils.hpp"
#include "common/webui_utils.hpp"

#ifdef MESOS_WEBUI

namespace mesos {
namespace internal {
namespace master {
namespace webui {

void start(const process::PID<Master>& master, const Configuration& conf)
{
  std::vector<std::string> args(3);
  args[0] = "--master_port=" + utils::stringify(master.port);
  args[1] = "--webui_port=" + conf.get("webui_port", "8080");
  args[2] = "--log_dir=" + conf.get("log_dir", FLAGS_log_dir);

  utils::webui::start(conf, "master/webui.py", args);
}

} // namespace webui {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // MESOS_WEBUI
