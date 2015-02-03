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

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>

#include "common/parse.hpp"

#include "logging/flags.hpp"

#include "messages/messages.hpp"

#include "slave/constants.hpp"

namespace mesos {
namespace internal {
namespace slave {

class Flags : public logging::Flags
{
public:
  Flags()
    : checkpoint(true)
  {
    add(&Flags::hostname,
        "hostname",
        "The hostname the slave should report.\n"
        "If left unset, the hostname is resolved from the IP address\n"
        "that the slave binds to.");

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
        "or 'external', or load an alternate isolator module using\n"
        "the --modules flag.",
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
        "Attributes of machine, in the form:\n"
        "rack:2 or 'rack:2;u:1'");

    add(&Flags::work_dir,
        "work_dir",
        "Directory path to place framework work directories\n",
        "/tmp/mesos");

    add(&Flags::launcher_dir, // TODO(benh): This needs a better name.
        "launcher_dir",
        "Directory path of Mesos binaries",
        PKGLIBEXECDIR);

    add(&Flags::hadoop_home,
        "hadoop_home",
        "Path to find Hadoop installed (for\n"
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
        "Directory path prepended to relative executor URIs",
        "");

    add(&Flags::registration_backoff_factor,
        "registration_backoff_factor",
        "Slave initially picks a random amount of time between [0, b], where\n"
        "b = registration_backoff_factor, to (re-)register with a new master.\n"
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
        "Amount of time to wait for an executor to shut down\n"
        "(e.g., 60s, 3min, etc). If the flag value is too small\n"
        "(less than 3s), there may not be enough time for the\n"
        "executor to react and can result in a hard shutdown.",
        EXECUTOR_SHUTDOWN_GRACE_PERIOD);

    add(&Flags::gc_delay,
        "gc_delay",
        "Maximum amount of time to wait before cleaning up\n"
        "executor directories (e.g., 3days, 2weeks, etc).\n"
        "Note that this delay may be shorter depending on\n"
        "the available disk usage.",
        GC_DELAY);

    add(&Flags::gc_disk_headroom,
        "gc_disk_headroom",
        "Adjust disk headroom used to calculate maximum executor\n"
        "directory age. Age is calculated by:\n"
        "gc_delay * max(0.0, (1.0 - gc_disk_headroom - disk usage))\n"
        "every --disk_watch_interval duration. gc_disk_headroom must\n"
        "be a value between 0.0 and 1.0",
        GC_DISK_HEADROOM);

    add(&Flags::disk_watch_interval,
        "disk_watch_interval",
        "Periodic time interval (e.g., 10secs, 2mins, etc)\n"
        "to check the overall disk usage managed by the slave.\n"
        "This drives the garbage collection of archived\n"
        "information and sandboxes.",
        DISK_WATCH_INTERVAL);

    add(&Flags::resource_monitoring_interval,
        "resource_monitoring_interval",
        "Periodic time interval for monitoring executor\n"
        "resource usage (e.g., 10secs, 1min, etc)",
        RESOURCE_MONITORING_INTERVAL);

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

    add(&Flags::cgroups_enable_cfs,
        "cgroups_enable_cfs",
        "Cgroups feature flag to enable hard limits on CPU resources\n"
        "via the CFS bandwidth limiting subfeature.\n",
        false);

    // TODO(antonl): Set default to true in future releases.
    add(&Flags::cgroups_limit_swap,
        "cgroups_limit_swap",
        "Cgroups feature flag to enable memory limits on both memory and\n"
        "swap instead of just memory.\n",
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

    add(&Flags::containerizers,
        "containerizers",
        "Comma separated list of containerizer implementations\n"
        "to compose in order to provide containerization.\n"
        "Available options are 'mesos', 'external', and\n"
        "'docker' (on Linux). The order the containerizers\n"
        "are specified is the order they are tried\n"
        "(--containerizers=mesos).\n",
        "mesos");

    add(&Flags::default_container_image,
        "default_container_image",
        "The default container image to use if not specified by a task,\n"
        "when using external containerizer.\n");

    // Docker containerizer flags.
    add(&Flags::docker,
        "docker",
        "The absolute path to the docker executable for docker\n"
        "containerizer.\n",
        "docker");

    add(&Flags::docker_sandbox_directory,
        "docker_sandbox_directory",
        "The absolute path for the directory in the container where the\n"
        "sandbox is mapped to.\n",
        "/mnt/mesos/sandbox");

    add(&Flags::docker_remove_delay,
        "docker_remove_delay",
        "The amount of time to wait before removing docker containers\n"
        "(e.g., 3days, 2weeks, etc).\n",
        DOCKER_REMOVE_DELAY);

    add(&Flags::default_container_info,
        "default_container_info",
        "JSON formatted ContainerInfo that will be included into\n"
        "any ExecutorInfo that does not specify a ContainerInfo.\n"
        "\n"
        "See the ContainerInfo protobuf in mesos.proto for\n"
        "the expected format.\n"
        "\n"
        "Example:\n"
        "{\n"
        "\"type\": \"MESOS\",\n"
        "\"volumes\": [\n"
        "  {\n"
        "    \"host_path\": \"./.private/tmp\",\n"
        "    \"container_path\": \"/tmp\",\n"
        "    \"mode\": \"RW\"\n"
        "  }\n"
        " ]\n"
        "}"
        );

    add(&Flags::docker_stop_timeout,
        "docker_stop_timeout",
        "The time as a duration for docker to wait after stopping an instance\n"
        "before it kills that instance.",
        Seconds(0));

#ifdef WITH_NETWORK_ISOLATOR
    add(&Flags::ephemeral_ports_per_container,
        "ephemeral_ports_per_container",
        "Number of ephemeral ports allocated to a container by the network\n"
        "isolator. This number has to be a power of 2. This flag is used\n"
        "for the 'network/port_mapping' isolator.",
        DEFAULT_EPHEMERAL_PORTS_PER_CONTAINER);

    add(&Flags::eth0_name,
        "eth0_name",
        "The name of the public network interface (e.g., eth0). If it is\n"
        "not specified, the network isolator will try to guess it based\n"
        "on the host default gateway. This flag is used for the\n"
        "'network/port_mapping' isolator.");

    add(&Flags::lo_name,
        "lo_name",
        "The name of the loopback network interface (e.g., lo). If it is\n"
        "not specified, the network isolator will try to guess it. This\n"
        "flag is used for the 'network/port_mapping' isolator.");

    add(&Flags::egress_rate_limit_per_container,
        "egress_rate_limit_per_container",
        "The limit of the egress traffic for each container, in Bytes/s.\n"
        "If not specified or specified as zero, the network isolator will\n"
        "impose no limits to containers' egress traffic throughput.\n"
        "This flag uses the Bytes type (defined in stout) and is used for\n"
        "the 'network/port_mapping' isolator.");

    add(&Flags::network_enable_socket_statistics_summary,
        "network_enable_socket_statistics_summary",
        "Whether to collect socket statistics summary for each container.\n"
        "This flag is used for the 'network/port_mapping' isolator.",
        false);

    add(&Flags::network_enable_socket_statistics_details,
        "network_enable_socket_statistics_details",
        "Whether to collect socket statistics details (e.g., TCP RTT) for\n"
        "each container. This flag is used for the 'network/port_mapping'\n"
        "isolator.",
        false);

#endif // WITH_NETWORK_ISOLATOR

    add(&Flags::container_disk_watch_interval,
        "container_disk_watch_interval",
        "The interval between disk quota checks for containers. This flag is\n"
        "used for the 'posix/disk' isolator.",
        Seconds(30));

    add(&Flags::enforce_container_disk_quota,
        "enforce_container_disk_quota",
        "Whether to enable disk quota enforcement for containers. This flag\n"
        "is used for the 'posix/disk' isolator.",
        false);

    // This help message for --modules flag is the same for
    // {master,slave,tests}/flags.hpp and should always be kept in
    // sync.
    // TODO(karya): Remove the JSON example and add reference to the
    // doc file explaining the --modules flag.
    add(&Flags::modules,
        "modules",
        "List of modules to be loaded and be available to the internal\n"
        "subsystems.\n"
        "\n"
        "Use --modules=filepath to specify the list of modules via a\n"
        "file containing a JSON formatted string. 'filepath' can be\n"
        "of the form 'file:///path/to/file' or '/path/to/file'.\n"
        "\n"
        "Use --modules=\"{...}\" to specify the list of modules inline.\n"
        "\n"
        "Example:\n"
        "{\n"
        "  \"libraries\": [\n"
        "    {\n"
        "      \"file\": \"/path/to/libfoo.so\",\n"
        "      \"modules\": [\n"
        "        {\n"
        "          \"name\": \"org_apache_mesos_bar\",\n"
        "          \"parameters\": [\n"
        "            {\n"
        "              \"key\": \"X\",\n"
        "              \"value\": \"Y\"\n"
        "            }\n"
        "          ]\n"
        "        },\n"
        "        {\n"
        "          \"name\": \"org_apache_mesos_baz\"\n"
        "        }\n"
        "      ]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"qux\",\n"
        "      \"modules\": [\n"
        "        {\n"
        "          \"name\": \"org_apache_mesos_norf\"\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}");

    add(&Flags::authenticatee,
        "authenticatee",
        "Authenticatee implementation to use when authenticating against the\n"
        "master. Use the default '" + DEFAULT_AUTHENTICATEE + "', or\n"
        "load an alternate authenticatee module using --modules.",
        DEFAULT_AUTHENTICATEE);

    add(&Flags::hooks,
        "hooks",
        "A comma separated list of hook modules to be\n"
        "installed inside the slave.");
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
  double gc_disk_headroom;
  Duration disk_watch_interval;
  Duration resource_monitoring_interval;
  // TODO(cmaloney): Remove checkpoint variable entirely, fixing tests
  // which depend upon it. See MESOS-444 for more details.
  bool checkpoint;
  std::string recover;
  Duration recovery_timeout;
  bool strict;
  Duration register_retry_interval_min;
#ifdef __linux__
  std::string cgroups_hierarchy;
  std::string cgroups_root;
  bool cgroups_enable_cfs;
  bool cgroups_limit_swap;
  Option<std::string> slave_subsystems;
  Option<std::string> perf_events;
  Duration perf_interval;
  Duration perf_duration;
#endif
  Option<std::string> credential;
  Option<std::string> containerizer_path;
  std::string containerizers;
  Option<std::string> default_container_image;
  std::string docker;
  std::string docker_sandbox_directory;
  Duration docker_remove_delay;
  Option<ContainerInfo> default_container_info;
  Duration docker_stop_timeout;
#ifdef WITH_NETWORK_ISOLATOR
  uint16_t ephemeral_ports_per_container;
  Option<std::string> eth0_name;
  Option<std::string> lo_name;
  Option<Bytes> egress_rate_limit_per_container;
  bool network_enable_socket_statistics_summary;
  bool network_enable_socket_statistics_details;
#endif
  Duration container_disk_watch_interval;
  bool enforce_container_disk_quota;
  Option<Modules> modules;
  std::string authenticatee;
  Option<std::string> hooks;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_FLAGS_HPP__
