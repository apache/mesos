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

#include <stout/duration.hpp>
#include <stout/option.hpp>

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

    add(&Flags::executor_shutdown_grace_period,
        "executor_shutdown_grace_period",
        "Amount of time to wait for an executor\n"
        "to shut down (e.g., 60secs, 3mins, etc)",
        EXECUTOR_SHUTDOWN_GRACE_PERIOD);

    add(&Flags::gc_delay,
        "gc_delay",
        "Maximum amount of time to wait before cleaning up\n"
        "executor directories (e.g., 3days, 2weeks, etc).\n"
        "Note that this delay may be shorter depending on\n"
        "the available disk usage.",
        GC_DELAY);

    add(&Flags::disk_watch_interval,
        "disk_watch_interval",
        "Periodic time interval (e.g., 10secs, 2mins, etc)\n"
        "to check the disk usage",
        DISK_WATCH_INTERVAL);

    add(&Flags::resource_monitoring_interval,
        "resource_monitoring_interval",
        "Periodic time interval for monitoring executor\n"
        "resource usage (e.g., 10secs, 1min, etc)",
        RESOURCE_MONITORING_INTERVAL);

    add(&Flags::checkpoint,
        "checkpoint",
        "Whether to checkpoint slave and frameworks information\n"
        "to disk. This enables a restarted slave to recover\n"
        "status updates and reconnect with (--recover=reconnect) or\n"
        "kill (--recover=kill) old executors",
        false);

    add(&Flags::recover,
        "recover",
        "Whether to recover status updates and reconnect with old executors.\n"
        "Valid values for 'recover' are\n"
        "reconnect: Reconnect with any old live executors.\n"
        "cleanup  : Kill any old live executors and exit.\n"
        "           Use this option when doing an incompatible slave\n"
        "           or executor upgrade!).\n"
        "NOTE: If checkpointed slave doesn't exist, no recovery is performed\n"
        "      and the slave registers with the master as a new slave.",
        "reconnect");

    add(&Flags::safe,
        "safe",
        "Whether to ignore (safe=false) any ambiguous errors during recovery.\n"
        "An ambiguous error is one where it is impossible to differentiate\n"
        "between a safe error that can be ignored and an unsafe error that "
        "cannot be.",
        true);

#ifdef __linux__
    add(&Flags::cgroups_hierarchy,
        "cgroups_hierarchy",
        "The path to the cgroups hierarchy root\n",
        "/cgroup");

    add(&Flags::cgroups_root,
        "cgroups_root",
        "Name of the root cgroup\n",
        "mesos");

    add(&Flags::cgroups_subsystems,
        "cgroups_subsystems",
        "List of subsystems to enable (e.g., 'cpu,freezer')\n",
        "cpu,memory,freezer");
#endif
  }

  Option<std::string> resources;
  Option<std::string> attributes;
  std::string work_dir;
  std::string launcher_dir;
  std::string webui_dir;
  std::string hadoop_home; // TODO(benh): Make an Option.
  bool switch_user;
  std::string frameworks_home;  // TODO(benh): Make an Option.
  Duration executor_shutdown_grace_period;
  Duration gc_delay;
  Duration disk_watch_interval;
  Duration resource_monitoring_interval;
  bool checkpoint;
  std::string recover;
  bool safe;
#ifdef __linux__
  std::string cgroups_hierarchy;
  std::string cgroups_root;
  std::string cgroups_subsystems;
#endif
};

} // namespace mesos {
} // namespace internal {
} // namespace slave {

#endif // __SLAVE_FLAGS_HPP__
