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

#ifndef __SLAVE_FLAGS_HPP__
#define __SLAVE_FLAGS_HPP__

#include <string>

#include "common/option.hpp"

#include "flags/flags.hpp"

#include "slave/constants.hpp"

namespace mesos {
namespace internal {
namespace slave {

class Flags : public virtual flags::FlagsBase
{
public:
  Flags()
  {
    // TODO(benh): Is there a way to specify units for the resources?
    add(&Flags::resources,
        "resources",
        "Total consumable resources per slave");

    add(&Flags::attributes,
      "attributes",
      "Attributes of machine");

    add(&Flags::work_dir,
        "work_dir",
        "Where to place framework work directories\n",
        "/tmp/mesos");

    add(&Flags::launcher_dir, // TODO(benh): This needs a better name.
        "launcher_dir",
        "Location of Mesos binaries",
        MESOS_LIBEXECDIR);

    add(&Flags::webui_dir,
        "webui_dir",
        "Location of the webui files/assets",
        MESOS_WEBUI_DIR);

    add(&Flags::webui_port,
        "webui_port",
        "Web UI port (deprecated)",
        8081);

    add(&Flags::hadoop_home,
        "hadoop_home",
        "Where to find Hadoop installed (for\n"
        "fetching framework executors from HDFS)\n"
        "(no default, look for HADOOP_HOME in\n"
        "environment or find hadoop on PATH)",
        "");

    add(&Flags::switch_user,
        "switch_user",
        "Whether to run tasks as the user who\n"
        "submitted them rather than the user running\n"
        "the slave (requires setuid permission)",
        true);

    add(&Flags::frameworks_home,
        "frameworks_home",
        "Directory prepended to relative executor URIs",
        "");

    add(&Flags::executor_shutdown_timeout_seconds,
        "executor_shutdown_timeout_seconds",
        "Amount of time (in seconds) to wait for\n"
        "an executor to shut down",
        EXECUTOR_SHUTDOWN_TIMEOUT_SECONDS);

    add(&Flags::gc_timeout_hours,
        "gc_timeout_hours",
        "Amount of time (in hours) to wait before\n"
        "cleaning up executor directories",
        GC_TIMEOUT_HOURS);
  }

  Option<std::string> resources;
  Option<std::string> attributes;
  std::string work_dir;
  std::string launcher_dir;
  std::string webui_dir;
  short webui_port;
  std::string hadoop_home; // TODO(benh): Make an Option.
  bool switch_user;
  std::string frameworks_home;  // TODO(benh): Make an Option.
  double executor_shutdown_timeout_seconds;
  double gc_timeout_hours;
};

} // namespace mesos {
} // namespace internal {
} // namespace slave {

#endif // __SLAVE_FLAGS_HPP__
