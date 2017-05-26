// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "slave/flags.hpp"

#include <stout/error.hpp>
#include <stout/flags.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <mesos/type_utils.hpp>

#include "common/http.hpp"
#include "common/parse.hpp"

#include "slave/constants.hpp"

#ifdef __linux__
#include "slave/containerizer/mesos/linux_launcher.hpp"
#endif // __linux__

#include "slave/containerizer/mesos/provisioner/constants.hpp"

using std::string;

mesos::internal::slave::Flags::Flags()
{
  add(&Flags::hostname,
      "hostname",
      "The hostname the agent should report.\n"
      "If left unset, the hostname is resolved from the IP address\n"
      "that the agent advertises; unless the user explicitly prevents\n"
      "that, using `--no-hostname_lookup`, in which case the IP itself\n"
      "is used.");

  add(&Flags::hostname_lookup,
      "hostname_lookup",
      "Whether we should execute a lookup to find out the server's hostname,\n"
      "if not explicitly set (via, e.g., `--hostname`).\n"
      "True by default; if set to `false` it will cause Mesos\n"
      "to use the IP address, unless the hostname is explicitly set.",
      true);

  add(&Flags::version,
      "version",
      "Show version and exit.",
      false);

  // TODO(benh): Is there a way to specify units for the resources?
  add(&Flags::resources,
      "resources",
      "Total consumable resources per agent. Can be provided in JSON format\n"
      "or as a semicolon-delimited list of key:value pairs, with the role\n"
      "optionally specified.\n"
      "\n"
      "As a key:value list:\n"
      "`name(role):value;name:value...`\n"
      "\n"
      "To use JSON, pass a JSON-formatted string or use\n"
      "`--resources=filepath` to specify the resources via a file containing\n"
      "a JSON-formatted string. `filepath` can be of the form\n"
      "`file:///path/to/file` or `/path/to/file`.\n"
      "\n"
      "Example JSON:\n"
      "[\n"
      "  {\n"
      "    \"name\": \"cpus\",\n"
      "    \"type\": \"SCALAR\",\n"
      "    \"scalar\": {\n"
      "      \"value\": 24\n"
      "    }\n"
      "  },\n"
      "  {\n"
      "    \"name\": \"mem\",\n"
      "    \"type\": \"SCALAR\",\n"
      "    \"scalar\": {\n"
      "      \"value\": 24576\n"
      "    }\n"
      "  }\n"
      "]");

  add(&Flags::isolation,
      "isolation",
      "Isolation mechanisms to use, e.g., `posix/cpu,posix/mem` (or \n"
      "`windows/cpu` if you are on Windows), or\n"
      "`cgroups/cpu,cgroups/mem`, or network/port_mapping\n"
      "(configure with flag: `--with-network-isolator` to enable),\n"
      "or `gpu/nvidia` for nvidia specific gpu isolation,\n"
      "or load an alternate isolator module using the `--modules`\n"
      "flag. Note that this flag is only relevant for the Mesos\n"
      "Containerizer.",
#ifndef __WINDOWS__
      "posix/cpu,posix/mem"
#else
      "windows/cpu"
#endif // __WINDOWS__
      );

  add(&Flags::launcher,
      "launcher",
      "The launcher to be used for Mesos containerizer. It could either be\n"
      "`linux` or `posix`. The Linux launcher is required for cgroups\n"
      "isolation and for any isolators that require Linux namespaces such as\n"
      "network, pid, etc. If unspecified, the agent will choose the Linux\n"
      "launcher if it's running as root on Linux.",
#ifdef __linux__
      LinuxLauncher::available() ? "linux" : "posix"
#elif defined(__WINDOWS__)
      "windows"
#else
      "posix"
#endif // __linux__
      );

  add(&Flags::image_providers,
      "image_providers",
      "Comma-separated list of supported image providers,\n"
      "e.g., `APPC,DOCKER`.");

  add(&Flags::image_provisioner_backend,
      "image_provisioner_backend",
      "Strategy for provisioning container rootfs from images,\n"
      "e.g., `aufs`, `bind`, `copy`, `overlay`.");

  add(&Flags::appc_simple_discovery_uri_prefix,
      "appc_simple_discovery_uri_prefix",
      "URI prefix to be used for simple discovery of appc images,\n"
      "e.g., `http://`, `https://`, `hdfs://<hostname>:9000/user/abc/cde`.",
      "http://");

  add(&Flags::appc_store_dir,
      "appc_store_dir",
      "Directory the appc provisioner will store images in.\n",
      path::join(os::temp(), "mesos", "store", "appc"));

  add(&Flags::docker_registry,
      "docker_registry",
      "The default url for pulling Docker images. It could either be a Docker\n"
      "registry server url (i.e: `https://registry.docker.io`), or a local\n"
      "path (i.e: `/tmp/docker/images`) in which Docker image archives\n"
      "(result of `docker save`) are stored.",
      "https://registry-1.docker.io");

  add(&Flags::docker_store_dir,
      "docker_store_dir",
      "Directory the Docker provisioner will store images in",
      path::join(os::temp(), "mesos", "store", "docker"));

  add(&Flags::docker_volume_checkpoint_dir,
      "docker_volume_checkpoint_dir",
      "The root directory where we checkpoint the information about docker\n"
      "volumes that each container uses.",
      "/var/run/mesos/isolators/docker/volume");

  add(&Flags::default_role,
      "default_role",
      "Any resources in the `--resources` flag that\n"
      "omit a role, as well as any resources that\n"
      "are not present in `--resources` but that are\n"
      "automatically detected, will be assigned to\n"
      "this role.",
      "*");

  add(&Flags::attributes,
      "attributes",
      "Attributes of the agent machine, in the form:\n"
      "`rack:2` or `rack:2;u:1`");

  add(&Flags::fetcher_cache_size,
      "fetcher_cache_size",
      "Size of the fetcher cache in Bytes.",
      DEFAULT_FETCHER_CACHE_SIZE);

  // By default the fetcher cache directory is held inside the work
  // directory, so everything can be deleted or archived in one swoop,
  // in particular during testing. However, a typical production
  // scenario is to use a separate cache volume. First, it is not meant
  // to be backed up. Second, you want to avoid that sandbox directories
  // and the cache directory can interfere with each other in
  // unpredictable ways by occupying shared space. So it is recommended
  // to set the cache directory explicitly.
  add(&Flags::fetcher_cache_dir,
      "fetcher_cache_dir",
      "Parent directory for fetcher cache directories\n"
      "(one subdirectory per agent).",
      path::join(os::temp(), "mesos", "fetch"));

  add(&Flags::work_dir,
      "work_dir",
      "Path of the agent work directory. This is where executor sandboxes\n"
      "will be placed, as well as the agent's checkpointed state in case of\n"
      "failover. Note that locations like `/tmp` which are cleaned\n"
      "automatically are not suitable for the work directory when running in\n"
      "production, since long-running agents could lose data when cleanup\n"
      "occurs. (Example: `/var/lib/mesos/agent`)");

  add(&Flags::runtime_dir,
      "runtime_dir",
      "Path of the agent runtime directory. This is where runtime data\n"
      "is stored by an agent that it needs to persist across crashes (but\n"
      "not across reboots). This directory will be cleared on reboot.\n"
      "(Example: `/var/run/mesos`)",
      []() -> string {
        Try<string> var = os::var();
        if (var.isSome()) {
#ifdef __WINDOWS__
          const string prefix(var.get());
#else
          const string prefix(path::join(var.get(), "run"));
#endif // __WINDOWS__

          // We check for access on the prefix because the remainder
          // of the directory structure is created by the agent later.
          Try<bool> access = os::access(prefix, R_OK | W_OK);
          if (access.isSome() && access.get()) {
#ifdef __WINDOWS__
            return path::join(prefix, "mesos", "runtime");
#else
            return path::join(prefix, "mesos");
#endif // __WINDOWS__
          }
        }

        // We provide a fallback path for ease of use in case `os::var()`
        // errors or if the directory is not accessible.
        return path::join(os::temp(), "mesos", "runtime");
      }());

  add(&Flags::launcher_dir, // TODO(benh): This needs a better name.
      "launcher_dir",
      "Directory path of Mesos binaries. Mesos looks for the fetcher,\n"
      "containerizer, and executor binary files under this directory.",
      PKGLIBEXECDIR);

  add(&Flags::hadoop_home,
      "hadoop_home",
      "Path to find Hadoop installed (for\n"
      "fetching framework executors from HDFS)\n"
      "(no default, look for `HADOOP_HOME` in\n"
      "environment or find hadoop on `PATH`)",
      "");

#ifndef __WINDOWS__
  add(&Flags::switch_user,
      "switch_user",
      "If set to `true`, the agent will attempt to run tasks as\n"
      "the `user` who submitted them (as defined in `FrameworkInfo`)\n"
      "(this requires `setuid` permission and that the given `user`\n"
      "exists on the agent).\n"
      "If the user does not exist, an error occurs and the task will fail.\n"
      "If set to `false`, tasks will be run as the same user as the Mesos\n"
      "agent process.\n"
      "NOTE: This feature is not yet supported on Windows agent, and\n"
      "therefore the flag currently does not exist on that platform.",
      true);
#endif // __WINDOWS__

  add(&Flags::http_heartbeat_interval,
      "http_heartbeat_interval",
      "This flag sets a heartbeat interval (e.g. '5secs', '10mins') for\n"
      "messages to be sent over persistent connections made against\n"
      "the agent HTTP API. Currently, this only applies to the\n"
      "'LAUNCH_NESTED_CONTAINER_SESSION' and 'ATTACH_CONTAINER_OUTPUT' calls.",
      Seconds(30));

  add(&Flags::frameworks_home,
      "frameworks_home",
      "Directory path prepended to relative executor URIs", "");

  add(&Flags::registration_backoff_factor,
      "registration_backoff_factor",
      "Agent initially picks a random amount of time between `[0, b]`, where\n"
      "`b = registration_backoff_factor`, to (re-)register with a new master.\n"
      "Subsequent retries are exponentially backed off based on this\n"
      "interval (e.g., 1st retry uses a random value between `[0, b * 2^1]`,\n"
      "2nd retry between `[0, b * 2^2]`, 3rd retry between `[0, b * 2^3]`,\n"
      "etc) up to a maximum of " + stringify(REGISTER_RETRY_INTERVAL_MAX),
      DEFAULT_REGISTRATION_BACKOFF_FACTOR);

  add(&Flags::authentication_backoff_factor,
      "authentication_backoff_factor",
      "After a failed authentication the agent picks a random amount of time\n"
      "between `[0, b]`, where `b = authentication_backoff_factor`, to\n"
      "authenticate with a new master. Subsequent retries are exponentially\n"
      "backed off based on this interval (e.g., 1st retry uses a random\n"
      "value between `[0, b * 2^1]`, 2nd retry between `[0, b * 2^2]`, 3rd\n"
      "retry between `[0, b * 2^3]`, etc up to a maximum of " +
          stringify(AUTHENTICATION_RETRY_INTERVAL_MAX),
      DEFAULT_AUTHENTICATION_BACKOFF_FACTOR);

  add(&Flags::executor_environment_variables,
      "executor_environment_variables",
      "JSON object representing the environment variables that should be\n"
      "passed to the executor, and thus subsequently task(s). By default this\n"
      "flag is none. Users have to define executor environment explicitly.\n"
      "Example:\n"
      "{\n"
      "  \"PATH\": \"/bin:/usr/bin\",\n"
      "  \"LD_LIBRARY_PATH\": \"/usr/local/lib\"\n"
      "}",
      [](const Option<JSON::Object>& object) -> Option<Error> {
        if (object.isSome()) {
          foreachvalue (const JSON::Value& value, object.get().values) {
            if (!value.is<JSON::String>()) {
              return Error("`executor_environment_variables` must "
                           "only contain string values");
            }
          }
        }
        return None();
      });

  add(&Flags::executor_registration_timeout,
      "executor_registration_timeout",
      "Amount of time to wait for an executor\n"
      "to register with the agent before considering it hung and\n"
      "shutting it down (e.g., 60secs, 3mins, etc)",
      EXECUTOR_REGISTRATION_TIMEOUT);

  add(&Flags::executor_reregistration_timeout,
      "executor_reregistration_timeout",
      "The timeout within which an executor is expected to re-register after\n"
      "the agent has restarted, before the agent considers it gone and shuts\n"
      "it down. Note that currently, the agent will not re-register with the\n"
      "master until this timeout has elapsed (see MESOS-7539).",
      EXECUTOR_REREGISTRATION_TIMEOUT,
      [](const Duration& value) -> Option<Error> {
        if (value > MAX_EXECUTOR_REREGISTRATION_TIMEOUT) {
          return Error("Expected `--executor_reregistration_timeout` "
                       "to be not more than " +
                       stringify(MAX_EXECUTOR_REREGISTRATION_TIMEOUT));
        }
        return None();
      });

  // TODO(bmahler): Remove this once v0 executors are no longer supported.
  add(&Flags::executor_reregistration_retry_interval,
      "executor_reregistration_retry_interval",
      "For PID-based executors, how long the agent waits before retrying\n"
      "the reconnect message sent to the executor during recovery.\n"
      "NOTE: Do not use this unless you understand the following\n"
      "(see MESOS-5332): PID-based executors using Mesos libraries >= 1.1.2\n"
      "always re-link with the agent upon receiving the reconnect message.\n"
      "This avoids the executor replying on a half-open TCP connection to\n"
      "the old agent (possible if netfilter is dropping packets,\n"
      "see: MESOS-7057). However, PID-based executors using Mesos\n"
      "libraries < 1.1.2 do not re-link and are therefore prone to\n"
      "replying on a half-open connection after the agent restarts. If we\n"
      "only send a single reconnect message, these \"old\" executors will\n"
      "reply on their half-open connection and receive a RST; without any\n"
      "retries, they will fail to reconnect and be killed by the agent once\n"
      "the executor re-registration timeout elapses. To ensure these \"old\"\n"
      "executors can reconnect in the presence of netfilter dropping\n"
      "packets, we introduced optional retries of the reconnect message.\n"
      "This results in \"old\" executors correctly establishing a link\n"
      "when processing the second reconnect message.");

  add(&Flags::executor_shutdown_grace_period,
      "executor_shutdown_grace_period",
      "Default amount of time to wait for an executor to shut down\n"
      "(e.g. 60secs, 3mins, etc). ExecutorInfo.shutdown_grace_period\n"
      "overrides this default. Note that the executor must not assume\n"
      "that it will always be allotted the full grace period, as the\n"
      "agent may decide to allot a shorter period, and failures / forcible\n"
      "terminations may occur.",
      DEFAULT_EXECUTOR_SHUTDOWN_GRACE_PERIOD);

#ifdef USE_SSL_SOCKET
  add(&Flags::executor_secret_key,
      "executor_secret_key",
      "Path to a file containing the key used when generating executor\n"
      "secrets. This flag is only available when Mesos is built with SSL\n"
      "support.");
#endif // USE_SSL_SOCKET

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
      "`gc_delay * max(0.0, (1.0 - gc_disk_headroom - disk usage))`\n"
      "every `--disk_watch_interval` duration. `gc_disk_headroom` must\n"
      "be a value between 0.0 and 1.0",
      GC_DISK_HEADROOM);

  add(&Flags::disk_watch_interval,
      "disk_watch_interval",
      "Periodic time interval (e.g., 10secs, 2mins, etc)\n"
      "to check the overall disk usage managed by the agent.\n"
      "This drives the garbage collection of archived\n"
      "information and sandboxes.",
      DISK_WATCH_INTERVAL);

  add(&Flags::container_logger,
      "container_logger",
      "The name of the container logger to use for logging container\n"
      "(i.e., executor and task) stdout and stderr. The default\n"
      "container logger writes to `stdout` and `stderr` files\n"
      "in the sandbox directory.");

  add(&Flags::recover,
      "recover",
      "Whether to recover status updates and reconnect with old executors.\n"
      "Valid values for `recover` are\n"
      "reconnect: Reconnect with any old live executors.\n"
      "cleanup  : Kill any old live executors and exit.\n"
      "           Use this option when doing an incompatible agent\n"
      "           or executor upgrade!).",
      "reconnect");

  add(&Flags::recovery_timeout,
      "recovery_timeout",
      "Amount of time allotted for the agent to recover. If the agent takes\n"
      "longer than recovery_timeout to recover, any executors that are\n"
      "waiting to reconnect to the agent will self-terminate.\n",
      RECOVERY_TIMEOUT);

  add(&Flags::strict,
      "strict",
      "If `strict=true`, any and all recovery errors are considered fatal.\n"
      "If `strict=false`, any expected errors (e.g., agent cannot recover\n"
      "information about an executor, because the agent died right before\n"
      "the executor registered.) during recovery are ignored and as much\n"
      "state as possible is recovered.\n",
      true);

  add(&Flags::max_completed_executors_per_framework,
      "max_completed_executors_per_framework",
      "Maximum number of completed executors per framework to store\n"
      "in memory.\n",
      DEFAULT_MAX_COMPLETED_EXECUTORS_PER_FRAMEWORK);

#ifdef __linux__
  add(&Flags::cgroups_hierarchy,
      "cgroups_hierarchy",
      "The path to the cgroups hierarchy root\n", "/sys/fs/cgroup");

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

  add(&Flags::cgroups_cpu_enable_pids_and_tids_count,
      "cgroups_cpu_enable_pids_and_tids_count",
      "Cgroups feature flag to enable counting of processes and threads\n"
      "inside a container.\n",
      false);

  add(&Flags::cgroups_net_cls_primary_handle,
      "cgroups_net_cls_primary_handle",
      "A non-zero, 16-bit handle of the form `0xAAAA`. This will be \n"
      "used as the primary handle for the net_cls cgroup.");

  add(&Flags::cgroups_net_cls_secondary_handles,
      "cgroups_net_cls_secondary_handles",
      "A range of the form 0xAAAA,0xBBBB, specifying the valid secondary\n"
      "handles that can be used with the primary handle. This will take\n"
      "effect only when the `--cgroups_net_cls_primary_handle is set.");

  add(&Flags::allowed_devices,
      "allowed_devices",
      "JSON array representing the devices that will be additionally\n"
      "whitelisted by cgroups devices subsystem. Noted that the following\n"
      "devices always be whitelisted by default:\n"
      "  * /dev/console\n"
      "  * /dev/tty0\n"
      "  * /dev/tty1\n"
      "  * /dev/pts/*\n"
      "  * /dev/ptmx\n"
      "  * /dev/net/tun\n"
      "  * /dev/null\n"
      "  * /dev/zero\n"
      "  * /dev/full\n"
      "  * /dev/tty\n"
      "  * /dev/urandom\n"
      "  * /dev/random\n"
      "This flag will take effect only when `cgroups/devices` is set in\n"
      "`--isolation` flag.\n"
      "Example:\n"
      "{\n"
      "  \"allowed_devices\": [\n"
      "    {\n"
      "      \"device\": {\n"
      "        \"path\": \"/path/to/device\"\n"
      "      },\n"
      "      \"access\": {\n"
      "        \"read\": true,\n"
      "        \"write\": false,\n"
      "        \"mknod\": false\n"
      "      }\n"
      "    }\n"
      "  ]\n"
      "}\n");

  add(&Flags::agent_subsystems,
      "agent_subsystems",
      flags::DeprecatedName("slave_subsystems"),
      "List of comma-separated cgroup subsystems to run the agent binary\n"
      "in, e.g., `memory,cpuacct`. The default is none.\n"
      "Present functionality is intended for resource monitoring and\n"
      "no cgroup limits are set, they are inherited from the root mesos\n"
      "cgroup.");

  add(&Flags::nvidia_gpu_devices,
      "nvidia_gpu_devices",
      "A comma-separated list of Nvidia GPU devices. When `gpus` is\n"
      "specified in the `--resources` flag, this flag determines which GPU\n"
      "devices will be made available. The devices should be listed as\n"
      "numbers that correspond to Nvidia's NVML device enumeration (as\n"
      "seen by running the command `nvidia-smi` on an Nvidia GPU\n"
      "equipped system).  The GPUs listed will only be isolated if the\n"
      "`--isolation` flag contains the string `gpu/nvidia`.");

  add(&Flags::perf_events,
      "perf_events",
      "List of command-separated perf events to sample for each container\n"
      "when using the perf_event isolator. Default is none.\n"
      "Run command `perf list` to see all events. Event names are\n"
      "sanitized by downcasing and replacing hyphens with underscores\n"
      "when reported in the PerfStatistics protobuf, e.g., `cpu-cycles`\n"
      "becomes `cpu_cycles`; see the PerfStatistics protobuf for all names.");

  add(&Flags::perf_interval,
      "perf_interval",
      "Interval between the start of perf stat samples. Perf samples are\n"
      "obtained periodically according to `perf_interval` and the most\n"
      "recently obtained sample is returned rather than sampling on\n"
      "demand. For this reason, `perf_interval` is independent of the\n"
      "resource monitoring interval",
      Seconds(60));

  add(&Flags::perf_duration,
      "perf_duration",
      "Duration of a perf stat sample. The duration must be less\n"
      "than the `perf_interval`.",
      Seconds(10));

  add(&Flags::revocable_cpu_low_priority,
      "revocable_cpu_low_priority",
      "Run containers with revocable CPU at a lower priority than\n"
      "normal containers (non-revocable cpu). Currently only\n"
      "supported by the cgroups/cpu isolator.",
      true);

  add(&Flags::systemd_enable_support,
      "systemd_enable_support",
      "Top level control of systemd support. When enabled, features such as\n"
      "executor life-time extension are enabled unless there is an explicit\n"
      "flag to disable these (see other flags). This should be enabled when\n"
      "the agent is launched as a systemd unit.",
      true);

  add(&Flags::systemd_runtime_directory,
      "systemd_runtime_directory",
      "The path to the systemd system run time directory\n",
      "/run/systemd/system");

  add(&Flags::allowed_capabilities,
      "allowed_capabilities",
      "JSON representation of system capabilities that the operator will\n"
      "allow for a task that will be run in a container launched by the\n"
      "containerizer (currently only supported in MesosContainerizer).\n"
      "This set overrides the default capabilities for the user and the\n"
      "capabilities requested by the framework.\n"
      "\n"
      "The net capability for a task running in the container would be:\n"
      "   ((F & A) & U)\n"
      "   where F = capabilities requested by the framework.\n"
      "         U = permitted capabilities for the agent process.\n"
      "         A = allowed capabilities specified by this flag.\n"
      "\n"
      "To set capabilities the agent should have the `SETPCAP` capability.\n"
      "\n"
      "This flag is effective iff `capabilities` isolation is enabled.\n"
      "When `capabilities` isolation is enabled, the absence of this flag\n"
      "would imply that the operator would allow ALL capabilities.\n"
      "\n"
      "Example:\n"
      "{\n"
      "   \"capabilities\": [\n"
      "       \"NET_RAW\",\n"
      "       \"SYS_ADMIN\"\n"
      "     ]\n"
      "}");
#endif

  add(&Flags::firewall_rules,
      "firewall_rules",
      "The value could be a JSON-formatted string of rules or a\n"
      "file path containing the JSON-formatted rules used in the endpoints\n"
      "firewall. Path must be of the form `file:///path/to/file`\n"
      "or `/path/to/file`.\n"
      "\n"
      "See the `Firewall` message in `flags.proto` for the expected format.\n"
      "\n"
      "Example:\n"
      "{\n"
      "  \"disabled_endpoints\": {\n"
      "    \"paths\": [\n"
      "      \"/files/browse\",\n"
      "      \"/metrics/snapshot\"\n"
      "    ]\n"
      "  }\n"
      "}");

  add(&Flags::credential,
      "credential",
      "Path to a JSON-formatted file containing the credential\n"
      "to use to authenticate with the master.\n"
      "Path could be of the form `file:///path/to/file` or `/path/to/file`."
      "\n"
      "Example:\n"
      "{\n"
      "  \"principal\": \"username\",\n"
      "  \"secret\": \"secret\"\n"
      "}");

  add(&Flags::acls,
      "acls",
      "The value could be a JSON-formatted string of ACLs\n"
      "or a file path containing the JSON-formatted ACLs used\n"
      "for authorization. Path could be of the form `file:///path/to/file`\n"
      "or `/path/to/file`.\n"
      "\n"
      "Note that if the `--authorizer` flag is provided with a value\n"
      "other than `" + string(DEFAULT_AUTHORIZER) + "`, the ACLs contents\n"
      "will be ignored.\n"
      "\n"
      "See the ACLs protobuf in acls.proto for the expected format.\n"
      "\n"
      "Example:\n"
      "{\n"
      "  \"get_endpoints\": [\n"
      "    {\n"
      "      \"principals\": { \"values\": [\"a\"] },\n"
      "      \"paths\": { \"values\": [\"/flags\", \"/monitor/statistics\"] }\n"
      "    }\n"
      "  ]\n"
      "}");

  add(&Flags::containerizers,
      "containerizers",
      "Comma-separated list of containerizer implementations\n"
      "to compose in order to provide containerization.\n"
      "Available options are `mesos` and `docker` (on Linux).\n"
      "The order the containerizers are specified is the order\n"
      "they are tried.\n",
      "mesos");

  // Docker containerizer flags.
  add(&Flags::docker,
      "docker",
      "The absolute path to the docker executable for docker\n"
      "containerizer.\n",
      "docker");

  add(&Flags::docker_remove_delay,
      "docker_remove_delay",
      "The amount of time to wait before removing docker containers\n"
      "(e.g., `3days`, `2weeks`, etc).\n",
      DOCKER_REMOVE_DELAY);

  add(&Flags::docker_kill_orphans,
      "docker_kill_orphans",
      "Enable docker containerizer to kill orphaned containers.\n"
      "You should consider setting this to false when you launch multiple\n"
      "agents in the same OS, to avoid one of the DockerContainerizer \n"
      "removing docker tasks launched by other agents.\n",
      true);

  add(&Flags::docker_mesos_image,
      "docker_mesos_image",
      "The Docker image used to launch this Mesos agent instance.\n"
      "If an image is specified, the docker containerizer assumes the agent\n"
      "is running in a docker container, and launches executors with\n"
      "docker containers in order to recover them when the agent restarts and\n"
      "recovers.\n");

  add(&Flags::docker_socket,
      "docker_socket",
      "Resource used by the agent and the executor to provide CLI access\n"
      "to the Docker daemon. On Unix, this is typically a path to a\n"
      "socket, such as '/var/run/docker.sock'. On Windows this must be a\n"
      "named pipe, such as '//./pipe/docker_engine'. NOTE: This must be\n"
      "the path used by the Docker image used to run the agent.\n",
      DEFAULT_DOCKER_HOST_RESOURCE);

  add(&Flags::docker_config,
      "docker_config",
      "The default docker config file for agent. Can be provided either as an\n"
      "absolute path pointing to the agent local docker config file, or as a\n"
      "JSON-formatted string. The format of the docker config file should be\n"
      "identical to docker's default one (e.g., either\n"
      "`$HOME/.docker/config.json` or `$HOME/.dockercfg`).\n"
      "Example JSON (`$HOME/.docker/config.json`):\n"
      "{\n"
      "  \"auths\": {\n"
      "    \"https://index.docker.io/v1/\": {\n"
      "      \"auth\": \"xXxXxXxXxXx=\",\n"
      "      \"email\": \"username@example.com\"\n"
      "    }\n"
      "  }\n"
      "}");

  add(&Flags::sandbox_directory,
      "sandbox_directory",
      "The absolute path for the directory in the container where the\n"
      "sandbox is mapped to.\n",
#ifndef __WINDOWS__
      "/mnt/mesos/sandbox"
#else
      "C:\\mesos\\sandbox"
#endif // __WINDOWS__
      );

  add(&Flags::default_container_info,
      "default_container_info",
      "JSON-formatted ContainerInfo that will be included into\n"
      "any ExecutorInfo that does not specify a ContainerInfo.\n"
      "\n"
      "See the ContainerInfo protobuf in mesos.proto for\n"
      "the expected format.\n"
      "\n"
      "Example:\n"
      "{\n"
      "  \"type\": \"MESOS\",\n"
      "  \"volumes\": [\n"
      "    {\n"
      "      \"host_path\": \".private/tmp\",\n"
      "      \"container_path\": \"/tmp\",\n"
      "      \"mode\": \"RW\"\n"
      "    }\n"
      "  ]\n"
      "}");

  // TODO(alexr): Remove this after the deprecation cycle (started in 1.0).
  add(&Flags::docker_stop_timeout,
      "docker_stop_timeout",
      "The time docker daemon waits after stopping a container before\n"
      "killing that container. This flag is deprecated; use task's kill\n"
      "policy instead.",
      Seconds(0));

#ifdef WITH_NETWORK_ISOLATOR
  add(&Flags::ephemeral_ports_per_container,
      "ephemeral_ports_per_container",
      "Number of ephemeral ports allocated to a container by the network\n"
      "isolator. This number has to be a power of 2. This flag is used\n"
      "for the `network/port_mapping` isolator.",
      DEFAULT_EPHEMERAL_PORTS_PER_CONTAINER);

  add(&Flags::eth0_name,
      "eth0_name",
      "The name of the public network interface (e.g., `eth0`). If it is\n"
      "not specified, the network isolator will try to guess it based\n"
      "on the host default gateway. This flag is used for the\n"
      "`network/port_mapping` isolator.");

  add(&Flags::lo_name,
      "lo_name",
      "The name of the loopback network interface (e.g., lo). If it is\n"
      "not specified, the network isolator will try to guess it. This\n"
      "flag is used for the `network/port_mapping` isolator.");

  add(&Flags::egress_rate_limit_per_container,
      "egress_rate_limit_per_container",
      "The limit of the egress traffic for each container, in Bytes/s.\n"
      "If not specified or specified as zero, the network isolator will\n"
      "impose no limits to containers' egress traffic throughput.\n"
      "This flag uses the Bytes type (defined in stout) and is used for\n"
      "the `network/port_mapping` isolator.");

  add(&Flags::egress_unique_flow_per_container,
      "egress_unique_flow_per_container",
      "Whether to assign an individual flow for each container for the\n"
      "egress traffic. This flag is used for the `network/port_mapping`\n"
      "isolator.",
      false);

  add(&Flags::egress_flow_classifier_parent,
      "egress_flow_classifier_parent",
      "When `egress_unique_flow_per_container` is enabled, we need to install\n"
      "a flow classifier (fq_codel) qdisc on egress side. This flag specifies\n"
      "where to install it in the hierarchy. By default, we install it at\n"
      "root.",
      "root");

  add(&Flags::network_enable_socket_statistics_summary,
      "network_enable_socket_statistics_summary",
      "Whether to collect socket statistics summary for each container.\n"
      "This flag is used for the `network/port_mapping` isolator.",
      false);

  add(&Flags::network_enable_socket_statistics_details,
      "network_enable_socket_statistics_details",
      "Whether to collect socket statistics details (e.g., TCP RTT) for\n"
      "each container. This flag is used for the `network/port_mapping`\n"
      "isolator.",
      false);

  add(&Flags::network_enable_snmp_statistics,
      "network_enable_snmp_statistics",
      "Whether to collect SNMP statistics details (e.g., TCPRetransSegs) for\n"
      "each container. This flag is used for the 'network/port_mapping'\n"
      "isolator.",
      false);

#endif // WITH_NETWORK_ISOLATOR

  add(&Flags::network_cni_plugins_dir,
      "network_cni_plugins_dir",
      "A search path for CNI plugin binaries. The `network/cni`\n"
      "isolator will find CNI plugins under these set of directories so that\n"
      "it can execute the plugins to add/delete container from the CNI\n"
      "networks.");

  add(&Flags::network_cni_config_dir,
      "network_cni_config_dir",
      "Directory path of the CNI network configuration files. For each\n"
      "network that containers launched in Mesos agent can connect to,\n"
      "the operator should install a network configuration file in JSON\n"
      "format in the specified directory.");

  add(&Flags::container_disk_watch_interval,
      "container_disk_watch_interval",
      "The interval between disk quota checks for containers. This flag is\n"
      "used for the `disk/du` isolator.",
      Seconds(15));

  // TODO(jieyu): Consider enabling this flag by default. Remember
  // to update the user doc if we decide to do so.
  add(&Flags::enforce_container_disk_quota,
      "enforce_container_disk_quota",
      "Whether to enable disk quota enforcement for containers. This flag\n"
      "is used for the `disk/du` isolator.",
      false);

  // This help message for --modules flag is the same for
  // {master,slave,sched,tests}/flags.[ch]pp and should always be kept in
  // sync.
  // TODO(karya): Remove the JSON example and add reference to the
  // doc file explaining the --modules flag.
  add(&Flags::modules,
      "modules",
      "List of modules to be loaded and be available to the internal\n"
      "subsystems.\n"
      "\n"
      "Use `--modules=filepath` to specify the list of modules via a\n"
      "file containing a JSON-formatted string. `filepath` can be\n"
      "of the form `file:///path/to/file` or `/path/to/file`.\n"
      "\n"
      "Use `--modules=\"{...}\"` to specify the list of modules inline.\n"
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

  // This help message for --modules_dir flag is the same for
  // {master,slave,sched,tests}/flags.[ch]pp and should always be kept in
  // sync.
  add(&Flags::modulesDir,
      "modules_dir",
      "Directory path of the module manifest files.\n"
      "The manifest files are processed in alphabetical order.\n"
      "(See --modules for more information on module manifest files)\n"
      "Cannot be used in conjunction with --modules.\n");

  add(&Flags::authenticatee,
      "authenticatee",
      "Authenticatee implementation to use when authenticating against the\n"
      "master. Use the default `" + string(DEFAULT_AUTHENTICATEE) + "`, or\n"
      "load an alternate authenticatee module using `--modules`.",
      DEFAULT_AUTHENTICATEE);

  add(&Flags::authorizer,
      "authorizer",
      "Authorizer implementation to use when authorizing actions that\n"
      "require it.\n"
      "Use the default `" + string(DEFAULT_AUTHORIZER) + "`, or\n"
      "load an alternate authorizer module using `--modules`.\n"
      "\n"
      "Note that if the `--authorizer` flag is provided with a value\n"
      "other than the default `" + string(DEFAULT_AUTHORIZER) + "`, the\n"
      "ACLs passed through the `--acls` flag will be ignored.",
      DEFAULT_AUTHORIZER);

  add(&Flags::http_authenticators,
      "http_authenticators",
      "HTTP authenticator implementation to use when handling requests to\n"
      "authenticated endpoints. Use the default\n"
      "`" + string(DEFAULT_BASIC_HTTP_AUTHENTICATOR) + "`, or load an\n"
      "alternate HTTP authenticator module using `--modules`.");

  add(&Flags::authenticate_http_readwrite,
      "authenticate_http_readwrite",
      "If `true`, only authenticated requests for read-write HTTP endpoints\n"
      "supporting authentication are allowed. If `false`, unauthenticated\n"
      "requests to such HTTP endpoints are also allowed.",
      false);

  add(&Flags::authenticate_http_readonly,
      "authenticate_http_readonly",
      "If `true`, only authenticated requests for read-only HTTP endpoints\n"
      "supporting authentication are allowed. If `false`, unauthenticated\n"
      "requests to such HTTP endpoints are also allowed.",
      false);

#ifdef USE_SSL_SOCKET
  add(&Flags::authenticate_http_executors,
      "authenticate_http_executors",
      "If `true`, only authenticated requests for the HTTP executor API are\n"
      "allowed. If `false`, unauthenticated requests are also allowed. This\n"
      "flag is only available when Mesos is built with SSL support.",
      false);
#endif // USE_SSL_SOCKET

  add(&Flags::http_credentials,
      "http_credentials",
      "Path to a JSON-formatted file containing credentials used to\n"
      "authenticate HTTP endpoints on the agent.\n"
      "Path can be of the form `file:///path/to/file` or `/path/to/file`.\n"
      "Example:\n"
      "{\n"
      "  \"credentials\": [\n"
      "    {\n"
      "      \"principal\": \"yoda\",\n"
      "      \"secret\": \"usetheforce\"\n"
      "    }\n"
      "  ]\n"
      "}");

  add(&Flags::hooks,
      "hooks",
      "A comma-separated list of hook modules to be\n"
      "installed inside the agent.");

  add(&Flags::resource_estimator,
      "resource_estimator",
      "The name of the resource estimator to use for oversubscription.");

  add(&Flags::qos_controller,
      "qos_controller",
      "The name of the QoS Controller to use for oversubscription.");

  add(&Flags::qos_correction_interval_min,
      "qos_correction_interval_min",
      "The agent polls and carries out QoS corrections from the QoS\n"
      "Controller based on its observed performance of running tasks.\n"
      "The smallest interval between these corrections is controlled by\n"
      "this flag.",
      Seconds(0));

  add(&Flags::oversubscribed_resources_interval,
      "oversubscribed_resources_interval",
      "The agent periodically updates the master with the current estimation\n"
      "about the total amount of oversubscribed resources that are allocated\n"
      "and available. The interval between updates is controlled by this\n"
      "flag.",
      Seconds(15));

  add(&Flags::master_detector,
      "master_detector",
      "The symbol name of the master detector to use. This symbol\n"
      "should exist in a module specified through the --modules flag.\n"
      "Cannot be used in conjunction with --master.");

#if ENABLE_XFS_DISK_ISOLATOR
  add(&Flags::xfs_project_range,
      "xfs_project_range",
      "The ranges of XFS project IDs to use for tracking directory quotas",
      "[5000-10000]");
#endif

  add(&Flags::http_command_executor,
      "http_command_executor",
      "The underlying executor library to be used for the command executor.\n"
      "If set to `true`, the command executor would use the HTTP based\n"
      "executor library to interact with the Mesos agent. If set to `false`,\n"
      "the driver based implementation would be used.\n"
      "NOTE: This flag is *experimental* and should not be used in\n"
      "production yet.",
      false);

  add(&Flags::ip,
      "ip",
      "IP address to listen on. This cannot be used in conjunction\n"
      "with `--ip_discovery_command`.");

  add(&Flags::port, "port", "Port to listen on.", SlaveInfo().port());

  add(&Flags::advertise_ip,
      "advertise_ip",
      "IP address advertised to reach this Mesos slave.\n"
      "The slave does not bind to this IP address.\n"
      "However, this IP address may be used to access this slave.");

  add(&Flags::advertise_port,
      "advertise_port",
      "Port advertised to reach this Mesos slave (along with\n"
      "`advertise_ip`). The slave does not bind to this port.\n"
      "However, this port (along with `advertise_ip`) may be used to\n"
      "access this slave.");

  add(&Flags::master,
      "master",
      "May be one of:\n"
      "  `host:port`\n"
      "  `zk://host1:port1,host2:port2,.../path`\n"
      "  `zk://username:password@host1:port1,host2:port2,.../path`\n"
      "  `file:///path/to/file` (where file contains one of the above)");

  add(&Flags::ip_discovery_command,
      "ip_discovery_command",
      "Optional IP discovery binary: if set, it is expected to emit\n"
      "the IP address which the slave will try to bind to.\n"
      "Cannot be used in conjunction with `--ip`.");
}
