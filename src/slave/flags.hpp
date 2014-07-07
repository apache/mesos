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

    add(&Flags::version,
        "version",
        "Show version and exit.",
        false);

    // TODO(benh): Is there a way to specify units for the resources?
    add(&Flags::resources,
        "resources",
        "Total consumable resources per slave, in\n"
        "the form 'name(role):value;name(role):value...'.");

    add(&Flags::isolation,
        "isolation",
        "Isolation mechanisms to use, e.g., 'posix/cpu,posix/mem', or\n"
        "'cgroups/cpu,cgroups/mem', or network/port_mapping\n"
        "(configure with flag: --with-network-isolator to enable),\n"
        "or 'external'.",
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

    add(&Flags::registration_backoff_factor,
        "registration_backoff_factor",
        "Slave initially picks a random amount of time between [0, b], where\n"
        "b = register_backoff_factor, to (re-)register with a new master.\n"
        "Subsequent retries are exponentially backed off based on this\n"
        "interval (e.g., 1st retry uses a random value between [0, b * 2^1],\n"
        "2nd retry between [0, b * 2^2], 3rd retry between [0, b * 2^3] etc)\n"
        "up to a maximum of " + stringify(REGISTER_RETRY_INTERVAL_MAX),
        REGISTRATION_BACKOFF_FACTOR);

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
        "kill (--recover=cleanup) old executors",
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

    add(&Flags::slave_subsystems,
        "slave_subsystems",
        "List of comma-separated cgroup subsystems to run the slave binary\n"
        "in, e.g., 'memory,cpuacct'. The default is none.\n"
        "Present functionality is intended for resource monitoring and\n"
        "no cgroup limits are set, they are inherited from the root mesos\n"
        "cgroup.");

    add(&Flags::perf_events,
        "perf_events",
        "List of command-separated perf events to sample for each container\n"
        "when using the perf_event isolator. Default is none.\n"
        "Run command 'perf list' to see all events. Event names are\n"
        "sanitized by downcasing and replacing hyphens with underscores\n"
        "when reported in the PerfStatistics protobuf, e.g., cpu-cycles\n"
        "becomes cpu_cycles; see the PerfStatistics protobuf for all names.");

    add(&Flags::perf_interval,
        "perf_interval",
        "Interval between the start of perf stat samples. Perf samples are\n"
        "obtained periodically according to perf_interval and the most\n"
        "recently obtained sample is returned rather than sampling on\n"
        "demand. For this reason, perf_interval is independent of the\n"
        "resource monitoring interval",
        Seconds(60));

    add(&Flags::perf_duration,
        "perf_duration",
        "Duration of a perf stat sample. The duration must be less\n"
        "that the perf_interval.",
        Seconds(10));
#endif

    add(&Flags::credential,
        "credential",
        "Either a path to a text with a single line\n"
        "containing 'principal' and 'secret' separated by "
        "whitespace.\n"
        "Or a path containing the JSON "
        "formatted information used for one credential.\n"
        "Path could be of the form 'file:///path/to/file' or '/path/to/file'."
        "\n"
        "Example:\n"
        "{\n"
        "    \"principal\": \"username\",\n"
        "    \"secret\": \"secret\",\n"
        "}");

    add(&Flags::containerizer_path,
        "containerizer_path",
        "The path to the external containerizer executable used when\n"
        "external isolation is activated (--isolation=external).\n");

    add(&Flags::default_container_image,
        "default_container_image",
        "The default container image to use if not specified by a task,\n"
        "when using external containerizer");

#ifdef WITH_NETWORK_ISOLATOR
    add(&Flags::ephemeral_ports_per_container,
        "ephemeral_ports_per_container",
        "Number of ephemeral ports allocated to a container by the network\n"
        "isolator. This number has to be a power of 2.\n",
        DEFAULT_EPHEMERAL_PORTS_PER_CONTAINER);

    add(&Flags::private_resources,
        "private_resources",
        "The resources that will be manged by the slave locally, and not\n"
        "exposed to Mesos master and frameworks. It shares the same format\n"
        "as the 'resources' flag. One example of such type of resources\n"
        "is ephemeral ports when port mapping network isolator is enabled.\n"
        "Use 'ports:[x-y]' to specify the ephemeral ports that will be\n"
        "locally managed.\n");

    add(&Flags::eth0_name,
        "eth0_name",
        "The name of the public network interface (e.g., eth0). If it is\n"
        "not specified, the network isolator will try to guess it based\n"
        "on the host default gateway.");

    add(&Flags::lo_name,
        "lo_name",
        "The name of the loopback network interface (e.g., lo). If it is\n"
        "not specified, the network isolator will try to guess it.");
#endif // WITH_NETWORK_ISOLATOR
  }

  bool version;
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
  Duration registration_backoff_factor;
  Duration executor_registration_timeout;
  Duration executor_shutdown_grace_period;
  Duration gc_delay;
  Duration disk_watch_interval;
  Duration resource_monitoring_interval;
  bool checkpoint;
  std::string recover;
  Duration recovery_timeout;
  bool strict;
  Duration register_retry_interval_min;
#ifdef __linux__
  std::string cgroups_hierarchy;
  std::string cgroups_root;
  Option<std::string> cgroups_subsystems;
  bool cgroups_enable_cfs;
  Option<std::string> slave_subsystems;
  Option<std::string> perf_events;
  Duration perf_interval;
  Duration perf_duration;
#endif
  Option<std::string> credential;
  Option<std::string> containerizer_path;
  Option<std::string> default_container_image;
#ifdef WITH_NETWORK_ISOLATOR
  uint16_t ephemeral_ports_per_container;
  Option<std::string> private_resources;
  Option<std::string> eth0_name;
  Option<std::string> lo_name;
#endif
};

} // namespace mesos {
} // namespace internal {
} // namespace slave {

#endif // __SLAVE_FLAGS_HPP__
