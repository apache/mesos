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
#include <stout/flags.hpp>
#include <stout/option.hpp>

#include "logging/flags.hpp"

#include "slave/constants.hpp"

namespace mesos {
namespace internal {
namespace slave {

class Flags : public logging::Flags
{
public:
  Flags()
  {
    add(&Flags::hostname,
        "hostname",
        "The hostname the slave should report.\n"
        "If left unset, system hostname will be used (recommended).");

    // TODO(benh): Is there a way to specify units for the resources?
    add(&Flags::resources,
        "resources",
        "Total consumable resources per slave, in\n"
        "the form 'name(role):value;name(role):value...'.");

    add(&Flags::isolation,
        "isolation",
        "Isolation mechanisms to use, e.g., 'posix/cpu,posix/mem'\n"
        "or 'cgroups/cpu,cgroups/mem'.",
        "posix/cpu,posix/mem");

    add(&Flags::default_role,
        "default_role",
        "Any resources in the --resources flag that\n"
        "omit a role, as well as any resources that\n"
        "are not present in --resources but that are\n"
        "automatically detected, will be assigned to\n"
        "this role.",
        "*");

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
        PKGLIBEXECDIR);

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

    add(&Flags::executor_registration_timeout,
        "executor_registration_timeout",
        "Amount of time to wait for an executor\n"
        "to register with the slave before considering it hung and\n"
        "shutting it down (e.g., 60secs, 3mins, etc)",
        EXECUTOR_REGISTRATION_TIMEOUT);

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

    // TODO(vinod): Consider killing this flag and always checkpoint.
    add(&Flags::checkpoint,
        "checkpoint",
        "Whether to checkpoint slave and frameworks information\n"
        "to disk. This enables a restarted slave to recover\n"
        "status updates and reconnect with (--recover=reconnect) or\n"
        "kill (--recover=kill) old executors",
        true);

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

    add(&Flags::recovery_timeout,
        "recovery_timeout",
        "Amount of time alloted for the slave to recover. If the slave takes\n"
        "longer than recovery_timeout to recover, any executors that are\n"
        "waiting to reconnect to the slave will self-terminate.\n"
        "NOTE: This flag is only applicable when checkpoint is enabled.\n",
        RECOVERY_TIMEOUT);

    add(&Flags::strict,
        "strict",
        "If strict=true, any and all recovery errors are considered fatal.\n"
        "If strict=false, any expected errors (e.g., slave cannot recover\n"
        "information about an executor, because the slave died right before\n"
        "the executor registered.) during recovery are ignored and as much\n"
        "state as possible is recovered.\n",
        true);

#ifdef __linux__
    add(&Flags::cgroups_hierarchy,
        "cgroups_hierarchy",
        "The path to the cgroups hierarchy root\n",
        "/sys/fs/cgroup");

    add(&Flags::cgroups_root,
        "cgroups_root",
        "Name of the root cgroup\n",
        "mesos");

    add(&Flags::cgroups_subsystems,
        "cgroups_subsystems",
        "This flag has been deprecated and is no longer used,\n"
        "please update your flags");

    add(&Flags::cgroups_enable_cfs,
        "cgroups_enable_cfs",
        "Cgroups feature flag to enable hard limits on CPU resources\n"
        "via the CFS bandwidth limiting subfeature.\n",
        false);
#endif
  }

  Option<std::string> hostname;
  Option<std::string> resources;
  std::string isolation;
  std::string default_role;
  Option<std::string> attributes;
  std::string work_dir;
  std::string launcher_dir;
  std::string hadoop_home; // TODO(benh): Make an Option.
  bool switch_user;
  std::string frameworks_home;  // TODO(benh): Make an Option.
  Duration executor_registration_timeout;
  Duration executor_shutdown_grace_period;
  Duration gc_delay;
  Duration disk_watch_interval;
  Duration resource_monitoring_interval;
  bool checkpoint;
  std::string recover;
  Duration recovery_timeout;
  bool strict;
#ifdef __linux__
  std::string cgroups_hierarchy;
  std::string cgroups_root;
  Option<std::string> cgroups_subsystems;
  bool cgroups_enable_cfs;
#endif
};

} // namespace mesos {
} // namespace internal {
} // namespace slave {

#endif // __SLAVE_FLAGS_HPP__
