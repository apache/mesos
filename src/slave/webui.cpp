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

#include "common/stringify.hpp"

#include "flags/flags.hpp"

#include "logging/flags.hpp"

#include "slave/flags.hpp"
#include "slave/webui.hpp"
#include "slave/slave.hpp"

#include "webui/webui.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace webui {

void start(const process::PID<Slave>& slave,
           const flags::Flags<logging::Flags, slave::Flags>& flags)
{
  std::vector<std::string> args(5);
  args[0] = "--slave_id=" + slave.id;
  args[1] = "--slave_port=" + stringify(slave.port);
  args[2] = "--webui_port=" + stringify(flags.webui_port);
  args[3] = "--log_dir=" + (flags.log_dir.isSome() ? flags.log_dir.get() : "");
  args[4] = "--work_dir=" + flags.work_dir;

  mesos::internal::webui::start(flags.webui_dir, "slave/webui.py", args);
}

} // namespace webui {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // MESOS_WEBUI
