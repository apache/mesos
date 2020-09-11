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


#include <mesos/type_utils.hpp>

#include <stout/flags.hpp>

#include "common/http.hpp"
#include "common/parse.hpp"
#include "master/constants.hpp"
#include "master/flags.hpp"

#include <mesos/allocator/allocator.hpp>

using std::string;

mesos::internal::master::Flags::Flags()
{
  add(&Flags::version,
      "version",
      "Show version and exit.",
      false);

  add(&Flags::hostname,
      "hostname",
      "The hostname the master should advertise in ZooKeeper.\n"
      "If left unset, the hostname is resolved from the IP address\n"
      "that the master advertises; unless the user explicitly prevents\n"
      "that, using `--no-hostname_lookup`, in which case the IP itself\n"
      "is used.");

  add(&Flags::hostname_lookup,
      "hostname_lookup",
      "Whether we should execute a lookup to find out the server's hostname,\n"
      "if not explicitly set (via, e.g., `--hostname`).\n"
      "True by default; if set to `false` it will cause Mesos\n"
      "to use the IP address, unless the hostname is explicitly set.",
      true);

  add(&Flags::root_submissions,
      "root_submissions",
      "Can root submit frameworks?",
      true);

  add(&Flags::work_dir,
      "work_dir",
      "Path of the master work directory. This is where the persistent\n"
      "information of the cluster will be stored. Note that locations like\n"
      "`/tmp` which are cleaned automatically are not suitable for the work\n"
      "directory when running in production, since long-running masters could\n"
      "lose data when cleanup occurs. (Example: `/var/lib/mesos/master`)");

  // TODO(bmahler): Consider removing `in_memory` as it was only
  // used before `replicated_log` was implemented.
  add(&Flags::registry,
      "registry",
      "Persistence strategy for the registry;\n"
      "available options are `replicated_log`, `in_memory` (for testing).",
      "replicated_log");

  // TODO(vinod): Instead of specifying the quorum size consider
  // specifying the number of masters or the list of masters.
  add(&Flags::quorum,
      "quorum",
      "The size of the quorum of replicas when using `replicated_log` based\n"
      "registry. It is imperative to set this value to be a majority of\n"
      "masters i.e., `quorum > (number of masters)/2`.\n"
      "NOTE: Not required if master is run in standalone mode (non-HA).");

  add(&Flags::zk_session_timeout,
      "zk_session_timeout",
      "ZooKeeper session timeout.",
      ZOOKEEPER_SESSION_TIMEOUT);

  // TODO(neilc): This flag is deprecated in 1.0 and will be removed 6
  // months later.
  add(&Flags::registry_strict,
      "registry_strict",
      "Whether the master will take actions based on the persistent\n"
      "information stored in the Registry.\n"
      "NOTE: This flag is *disabled* and will be removed in a future\n"
      "version of Mesos.",
      false,
      [](bool value) -> Option<Error> {
        if (value) {
          return Error("Support for '--registry_strict' has been "
                       "disabled and will be removed in a future "
                       "version of Mesos");
        }
        return None();
      });

  add(&Flags::registry_fetch_timeout,
      "registry_fetch_timeout",
      "Duration of time to wait in order to fetch data from the registry\n"
      "after which the operation is considered a failure.",
      Seconds(60));

  add(&Flags::registry_store_timeout,
      "registry_store_timeout",
      "Duration of time to wait in order to store data in the registry\n"
      "after which the operation is considered a failure.",
      Seconds(20));

  add(&Flags::log_auto_initialize,
      "log_auto_initialize",
      "Whether to automatically initialize the replicated log used for the\n"
      "registry. If this is set to false, the log has to be manually\n"
      "initialized when used for the very first time.",
      true);

  add(&Flags::agent_reregister_timeout,
      "agent_reregister_timeout",
      flags::DeprecatedName("slave_reregister_timeout"),
      "The timeout within which an agent is expected to reregister.\n"
      "Agents reregister when they become disconnected from the master\n"
      "or when a new master is elected as the leader. Agents that do not\n"
      "reregister within the timeout will be marked unreachable in the\n"
      "registry; if/when the agent reregisters with the master, any\n"
      "non-partition-aware tasks running on the agent will be terminated.\n"
      "NOTE: This value has to be at least " +
        stringify(MIN_AGENT_REREGISTER_TIMEOUT) + ".",
      MIN_AGENT_REREGISTER_TIMEOUT);

  // TODO(bmahler): Add a `Percentage` abstraction for flags.
  // TODO(bmahler): Add a `--production` flag for production defaults.
  add(&Flags::recovery_agent_removal_limit,
      "recovery_agent_removal_limit",
      flags::DeprecatedName("recovery_slave_removal_limit"),
      "For failovers, limit on the percentage of agents that can be removed\n"
      "from the registry *and* shutdown after the re-registration timeout\n"
      "elapses. If the limit is exceeded, the master will fail over rather\n"
      "than remove the agents.\n"
      "This can be used to provide safety guarantees for production\n"
      "environments. Production environments may expect that across master\n"
      "failovers, at most a certain percentage of agents will fail\n"
      "permanently (e.g. due to rack-level failures).\n"
      "Setting this limit would ensure that a human needs to get\n"
      "involved should an unexpected widespread failure of agents occur\n"
      "in the cluster.\n"
      "Values: [0%-100%]",
      stringify(RECOVERY_AGENT_REMOVAL_PERCENT_LIMIT * 100.0) + "%");

  // TODO(vinod): Add a `Rate` abstraction in stout and the
  // corresponding parser for flags.
  add(&Flags::agent_removal_rate_limit,
      "agent_removal_rate_limit",
      flags::DeprecatedName("slave_removal_rate_limit"),
      "The maximum rate (e.g., `1/10mins`, `2/3hrs`, etc) at which agents\n"
      "will be removed from the master when they fail health checks.\n"
      "By default, agents will be removed as soon as they fail the health\n"
      "checks. The value is of the form `(Number of agents)/(Duration)`.");

  add(&Flags::webui_dir,
      "webui_dir",
      "Directory path of the webui files/assets",
      PKGDATADIR "/webui");

  add(&Flags::whitelist,
      "whitelist",
      "Path to a file which contains a list of agents (one per line) to\n"
      "advertise offers for. The file is watched and periodically re-read to\n"
      "refresh the agent whitelist. By default there is no whitelist: all\n"
      "machines are accepted. Path can be of the form\n"
      "`file:///path/to/file` or `/path/to/file`.\n");

  add(&Flags::role_sorter,
      "role_sorter",
      flags::DeprecatedName("user_sorter"),
      "Policy to use for allocating resources between roles when\n"
      "allocating up to quota guarantees as well as when allocating\n"
      "up to quota limits. May be one of: [drf, random]",
      "drf");

  add(&Flags::framework_sorter,
      "framework_sorter",
      "Policy to use for allocating resources between a given role's\n"
      "frameworks. Options are the same as for `--user_sorter`.",
      "drf");

  add(&Flags::allocation_interval,
      "allocation_interval",
      "Amount of time to wait between performing\n"
      " (batch) allocations (e.g., 500ms, 1sec, etc).",
      DEFAULT_ALLOCATION_INTERVAL);

  add(&Flags::cluster,
      "cluster",
      "Human readable name for the cluster, displayed in the webui.");

  add(&Flags::roles,
      "roles",
      "A comma-separated list of the allocation roles that frameworks\n"
      "in this cluster may belong to. This flag is deprecated;\n"
      "if it is not specified, any role name can be used.");

  add(&Flags::weights,
      "weights",
      "A comma-separated list of role/weight pairs of the form\n"
      "`role=weight,role=weight`. Weights can be used to control the\n"
      "relative share of cluster resources that is offered to different\n"
      "roles. This flag is deprecated. Instead, operators should configure\n"
      "weights dynamically using the `/weights` HTTP endpoint.");

  // TODO(adam-mesos): Deprecate --authenticate for --authenticate_frameworks.
  // See MESOS-4386 for details.
  add(&Flags::authenticate_frameworks,
      "authenticate_frameworks",
      flags::DeprecatedName("authenticate"),
      "If `true`, only authenticated frameworks are allowed to register. If\n"
      "`false`, unauthenticated frameworks are also allowed to register. For\n"
      "HTTP based frameworks use the `--authenticate_http_frameworks` flag.",
      false);

  add(&Flags::authenticate_agents,
      "authenticate_agents",
      flags::DeprecatedName("authenticate_slaves"),
      "If `true`, only authenticated agents are allowed to register.\n"
      "If `false`, unauthenticated agents are also allowed to register.",
      false);

  // TODO(bmahler): Ideally, we remove this v0-style authentication
  // in favor of just using HTTP authentication at the libprocess
  // layer.
  add(&Flags::authentication_v0_timeout,
      "authentication_v0_timeout",
      "The timeout within which an authentication is expected\n"
      "to complete against a v0 framework or agent. This does not\n"
      "apply to the v0 or v1 HTTP APIs.",
      DEFAULT_AUTHENTICATION_V0_TIMEOUT);

  // TODO(zhitao): Remove deprecated `--authenticate_http` flag name after
  // the deprecation cycle which started with Mesos 1.0.
  add(&Flags::authenticate_http_readwrite,
      "authenticate_http_readwrite",
      flags::DeprecatedName("authenticate_http"),
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

  add(&Flags::authenticate_http_frameworks,
      "authenticate_http_frameworks",
      "If `true`, only authenticated HTTP frameworks are allowed to register.\n"
      "If `false`, HTTP frameworks are not authenticated.",
      false);

  add(&Flags::credentials,
      "credentials",
      "Path to a JSON-formatted file containing credentials.\n"
      "Path could be of the form `file:///path/to/file` or `/path/to/file`."
      "\n"
      "Example:\n"
      "{\n"
      "  \"credentials\": [\n"
      "    {\n"
      "      \"principal\": \"sherman\",\n"
      "      \"secret\": \"kitesurf\"\n"
      "    }\n"
      "  ]\n"
      "}");

  add(&Flags::acls,
      "acls",
      "The value could be a JSON-formatted string of ACLs\n"
      "or a file path containing the JSON-formatted ACLs used\n"
      "for authorization. Path could be of the form `file:///path/to/file`\n"
      "or `/path/to/file`.\n"
      "\n"
      "Note that if the flag `--authorizers` is provided with a value\n"
      "different than `" + string(DEFAULT_AUTHORIZER) + "`, the ACLs contents\n"
      "will be ignored.\n"
      "\n"
      "See the ACLs protobuf in acls.proto for the expected format.\n"
      "\n"
      "Example:\n"
      "{\n"
      "  \"register_frameworks\": [\n"
      "    {\n"
      "      \"principals\": { \"type\": \"ANY\" },\n"
      "      \"roles\": { \"values\": [\"a\"] }\n"
      "    }\n"
      "  ],\n"
      "  \"run_tasks\": [\n"
      "    {\n"
      "      \"principals\": { \"values\": [\"a\", \"b\"] },\n"
      "      \"users\": { \"values\": [\"c\"] }\n"
      "    }\n"
      "  ],\n"
      "  \"teardown_frameworks\": [\n"
      "    {\n"
      "      \"principals\": { \"values\": [\"a\", \"b\"] },\n"
      "      \"framework_principals\": { \"values\": [\"c\"] }\n"
      "    }\n"
      "  ],\n"
      "  \"set_quotas\": [\n"
      "    {\n"
      "      \"principals\": { \"values\": [\"a\"] },\n"
      "      \"roles\": { \"values\": [\"a\", \"b\"] }\n"
      "    }\n"
      "  ],\n"
      "  \"remove_quotas\": [\n"
      "    {\n"
      "      \"principals\": { \"values\": [\"a\"] },\n"
      "      \"quota_principals\": { \"values\": [\"a\"] }\n"
      "    }\n"
      "  ]\n"
      "}");

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
      "  \"disabled_endpoints\" : {\n"
      "    \"paths\" : [\n"
      "      \"/files/browse\",\n"
      "      \"/metrics/snapshot\"\n"
      "    ]\n"
      "  }\n"
      "}");

  add(&Flags::rate_limits,
      "rate_limits",
      "The value could be a JSON-formatted string of rate limits\n"
      "or a file path containing the JSON-formatted rate limits used\n"
      "for framework rate limiting.\n"
      "Path could be of the form `file:///path/to/file`\n"
      "or `/path/to/file`.\n"
      "\n"
      "See the RateLimits protobuf in mesos.proto for the expected format.\n"
      "\n"
      "Example:\n"
      "{\n"
      "  \"limits\": [\n"
      "    {\n"
      "      \"principal\": \"foo\",\n"
      "      \"qps\": 55.5\n"
      "    },\n"
      "    {\n"
      "      \"principal\": \"bar\"\n"
      "    }\n"
      "  ],\n"
      "  \"aggregate_default_qps\": 33.3\n"
      "}");

#ifdef ENABLE_PORT_MAPPING_ISOLATOR
  add(&Flags::max_executors_per_agent,
      "max_executors_per_agent",
      flags::DeprecatedName("max_executors_per_slave"),
      "Maximum number of executors allowed per agent. The network\n"
      "monitoring/isolation technique imposes an implicit resource\n"
      "acquisition on each executor (# ephemeral ports), as a result\n"
      "one can only run a certain number of executors on each agent.");
#endif // ENABLE_PORT_MAPPING_ISOLATOR

  // TODO(karya): When we have optimistic offers, this will only
  // benefit frameworks that accidentally lose an offer.
  add(&Flags::offer_timeout,
      "offer_timeout",
      "Duration of time before an offer is rescinded from a framework.\n"
      "This helps fairness when running frameworks that hold on to offers,\n"
      "or frameworks that accidentally drop offers.\n"
      "If not set, offers do not timeout.");

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
      "}\n\n"
      "Cannot be used in conjunction with --modules_dir.\n");

  // This help message for --modules_dir flag is the same for
  // {master,slave,sched,tests}/flags.[ch]pp and should always be kept in
  // sync.
  add(&Flags::modulesDir,
      "modules_dir",
      "Directory path of the module manifest files.\n"
      "The manifest files are processed in alphabetical order.\n"
      "(See --modules for more information on module manifest files).\n"
      "Cannot be used in conjunction with --modules.\n");

  add(&Flags::authenticators,
      "authenticators",
      "Authenticator implementation to use when authenticating frameworks\n"
      "and/or agents. Use the default `" + string(DEFAULT_AUTHENTICATOR) + "`\n"
      "or load an alternate authenticator module using `--modules`.",
      DEFAULT_AUTHENTICATOR);

  add(&Flags::allocator,
      "allocator",
      "Allocator to use for resource allocation to frameworks.\n"
      "Use the default `" + string(DEFAULT_ALLOCATOR) + "` allocator, or\n"
      "load an alternate allocator module using `--modules`.",
      DEFAULT_ALLOCATOR);

  add(&Flags::allocator_agent_recovery_factor,
      "allocator_agent_recovery_factor",
      "Minimum fraction of known agents re-registered after leader election\n"
      "for the allocator to start generating offers.",
      mesos::allocator::DEFAULT_ALLOCATOR_AGENT_RECOVERY_FACTOR);

  add(&Flags::allocator_recovery_timeout,
      "allocator_recovery_timeout",
      "Maximum time to wait before sending offers after a leader\n"
      "re-election.",
      mesos::allocator::DEFAULT_ALLOCATOR_RECOVERY_TIMEOUT);

  add(&Flags::fair_sharing_excluded_resource_names,
      "fair_sharing_excluded_resource_names",
      "A comma-separated list of the resource names (e.g. 'gpus')\n"
      "that will be excluded from fair sharing constraints.\n"
      "This may be useful in cases where the fair sharing\n"
      "implementation currently has limitations. E.g. See the\n"
      "problem of \"scarce\" resources:\n"
      "  http://www.mail-archive.com/dev@mesos.apache.org/msg35631.html\n"
      "  https://issues.apache.org/jira/browse/MESOS-5377");

  add(&Flags::filter_gpu_resources,
      "filter_gpu_resources",
      "When set to true, this flag will cause the mesos master to\n"
      "filter all offers from agents with GPU resources by only sending\n"
      "them to frameworks that opt into the `GPU_RESOURCES` framework\n"
      "capability. When set to false, this flag will cause the master\n"
      "to not filter offers from agents with GPU resources, and\n"
      "indiscriminately send them to all frameworks whether they set\n"
      "the `GPU_RESOURCES` capability or not. This flag is meant as a\n"
      "temporary workaround towards the eventual deprecation of the\n"
      "`GPU_RESOURCES` capability. Please see the following for more\n"
      "information:\n"
      "  https://www.mail-archive.com/dev@mesos.apache.org/msg37571.html\n"
      "  https://issues.apache.org/jira/browse/MESOS-7576",
      true);

  add(&Flags::min_allocatable_resources,
      "min_allocatable_resources",
      "One or more sets of resource quantities that define the minimum\n"
      "allocatable resources for the allocator. The allocator will only offer\n"
      "resources that meets the quantity requirement of at least one of the\n"
      "specified sets. For `SCALAR` type resources, its quantity is its\n"
      "scalar value. For `RANGES` and `SET` type, their quantities are the\n"
      "number of different instances in the range or set. For example,\n"
      "`range:[1-5]` has a quantity of 5 and `set:{a,b}` has a quantity of 2.\n"
      "The resources in each set should be delimited by semicolons (acting as\n"
      "logical AND), and each set should be delimited by the pipe character\n"
      "(acting as logical OR).\n"
      "(Example: `disk:1|cpus:1;mem:32;ports:1` configures the allocator to\n"
      "only offer resources if they contain a disk resource of at least\n"
      "1 megabyte, or if they at least contain 1 cpu, 32 megabytes of memory\n"
      "and 1 port.)\n",
      "cpus:" + stringify(MIN_CPUS) +
        "|mem:" + stringify((double)MIN_MEM.bytes() / Bytes::MEGABYTES));

  add(&Flags::hooks,
      "hooks",
      "A comma-separated list of hook modules to be\n"
      "installed inside master.");

  add(&Flags::agent_ping_timeout,
      "agent_ping_timeout",
      flags::DeprecatedName("slave_ping_timeout"),
      "The timeout within which an agent is expected to respond to a\n"
      "ping from the master. Agents that do not respond within\n"
      "max_agent_ping_timeouts ping retries will be marked unreachable.\n"
      "NOTE: The total ping timeout (`agent_ping_timeout` multiplied by\n"
      "`max_agent_ping_timeouts`) should be greater than the ZooKeeper\n"
      "session timeout to prevent useless re-registration attempts.\n",
      DEFAULT_AGENT_PING_TIMEOUT,
      [](const Duration& value) -> Option<Error> {
        if (value < Seconds(1) || value > Minutes(15)) {
          return Error("Expected `--agent_ping_timeout` to be between " +
                       stringify(Seconds(1)) + " and " +
                       stringify(Minutes(15)));
        }
        return None();
      });

  add(&Flags::max_agent_ping_timeouts,
      "max_agent_ping_timeouts",
      flags::DeprecatedName("max_slave_ping_timeouts"),
      "The number of times an agent can fail to respond to a\n"
      "ping from the master. Agents that do not respond within\n"
      "`max_agent_ping_timeouts` ping retries will be marked unreachable.\n",
      DEFAULT_MAX_AGENT_PING_TIMEOUTS,
      [](size_t value) -> Option<Error> {
        if (value < 1) {
          return Error("Expected `--max_agent_ping_timeouts` to be at least 1");
        }
        return None();
      });

  add(&Flags::authorizers,
      "authorizers",
      "Authorizer implementation to use when authorizing actions that\n"
      "require it.\n"
      "Use the default `" + string(DEFAULT_AUTHORIZER) + "`, or\n"
      "load an alternate authorizer module using `--modules`.\n"
      "\n"
      "Note that if the flag `--authorizers` is provided with a value\n"
      "different than the default `" + string(DEFAULT_AUTHORIZER) + "`, the\n"
      "ACLs passed through the `--acls` flag will be ignored.\n"
      "\n"
      "Currently there is no support for multiple authorizers.",
      DEFAULT_AUTHORIZER);

  add(&Flags::http_authenticators,
      "http_authenticators",
      "HTTP authenticator implementation to use when handling requests to\n"
      "authenticated endpoints. Use the default "
      "`" + string(DEFAULT_BASIC_HTTP_AUTHENTICATOR) + "`, or load an\n"
      "alternate HTTP authenticator module using `--modules`.\n"
      "\n"
      "Currently there is no support for multiple HTTP authenticators.",
      DEFAULT_BASIC_HTTP_AUTHENTICATOR);

  add(&Flags::http_framework_authenticators,
      "http_framework_authenticators",
      "HTTP authenticator implementation to use when authenticating HTTP\n"
      "frameworks. Use the \n"
      "`" + string(DEFAULT_BASIC_HTTP_AUTHENTICATOR) + "` authenticator or\n"
      "load an alternate authenticator module using `--modules`.\n"
      "Must be used in conjunction with `--http_authenticate_frameworks`.\n"
      "\n"
      "Currently there is no support for multiple HTTP framework\n"
      "authenticators.");

  add(&Flags::max_operator_event_stream_subscribers,
      "max_operator_event_stream_subscribers",
      "Maximum number of simultaneous subscribers to the master's operator\n"
      "event stream. If new connections bring the total number of subscribers\n"
      "over this value, older connections will be closed by the master.\n"
      "\n"
      "This flag should generally not be changed unless the operator is\n"
      "mitigating known problems with their network setup, such as\n"
      "clients/proxies that do not close connections to the master.",
      DEFAULT_MAX_OPERATOR_EVENT_STREAM_SUBSCRIBERS);

  add(&Flags::max_completed_frameworks,
      "max_completed_frameworks",
      "Maximum number of completed frameworks to store in memory.",
      DEFAULT_MAX_COMPLETED_FRAMEWORKS);

  add(&Flags::max_completed_tasks_per_framework,
      "max_completed_tasks_per_framework",
      "Maximum number of completed tasks per framework to store in memory.",
      DEFAULT_MAX_COMPLETED_TASKS_PER_FRAMEWORK);

  add(&Flags::max_unreachable_tasks_per_framework,
      "max_unreachable_tasks_per_framework",
      "Maximum number of unreachable tasks per framework to store in memory.",
      DEFAULT_MAX_UNREACHABLE_TASKS_PER_FRAMEWORK);

  add(&Flags::master_contender,
      "master_contender",
      "The symbol name of the master contender to use.\n"
      "This symbol should exist in a module specified through\n"
      "the --modules flag. Cannot be used in conjunction with --zk.\n"
      "Must be used in conjunction with --master_detector.");

  add(&Flags::master_detector,
      "master_detector",
      "The symbol name of the master detector to use. This symbol\n"
      "should exist in a module specified through the --modules flag.\n"
      "Cannot be used in conjunction with --zk.\n"
      "Must be used in conjunction with --master_contender.");

  add(&Flags::registry_gc_interval,
      "registry_gc_interval",
      "How often to garbage collect the registry. The current leading\n"
      "master will periodically discard information from the registry.\n"
      "How long registry state is retained is controlled by other\n"
      "parameters (e.g., registry_max_agent_age, registry_max_agent_count);\n"
      "this parameter controls how often the master will examine the\n"
      "registry to see if data should be discarded.",
      DEFAULT_REGISTRY_GC_INTERVAL);

  add(&Flags::registry_max_agent_age,
      "registry_max_agent_age",
      "Maximum length of time to store information in the registry about\n"
      "agents that are not currently connected to the cluster. This\n"
      "information allows frameworks to determine the status of unreachable\n"
      "and gone agents. Note that the registry always stores\n"
      "information on all connected agents. If there are more than\n"
      "`registry_max_agent_count` partitioned/gone agents, agent\n"
      "information may be discarded from the registry sooner than indicated\n"
      "by this parameter.",
      DEFAULT_REGISTRY_MAX_AGENT_AGE);

  add(&Flags::registry_max_agent_count,
      "registry_max_agent_count",
      "Maximum number of partitioned/gone agents to store in the\n"
      "registry. This information allows frameworks to determine the status\n"
      "of disconnected agents. Note that the registry always stores\n"
      "information about all connected agents. See also the\n"
      "`registry_max_agent_age` flag.",
      DEFAULT_REGISTRY_MAX_AGENT_COUNT);

  add(&Flags::ip,
      "ip",
      "IP address to listen on. This cannot be used in conjunction\n"
      "with `--ip_discovery_command`.");

  add(&Flags::port, "port", "Port to listen on.", MasterInfo().port());

  add(&Flags::advertise_ip,
      "advertise_ip",
      "IP address advertised to reach this Mesos master.\n"
      "The master does not bind using this IP address.\n"
      "However, this IP address may be used to access this master.");

  add(&Flags::advertise_port,
      "advertise_port",
      "Port advertised to reach Mesos master (along with\n"
      "`advertise_ip`). The master does not bind to this port.\n"
      "However, this port (along with `advertise_ip`) may be used to\n"
      "access this master.");

  // TODO(bevers): Switch the default to `true` after gathering some
  // real-world experience.
  add(&Flags::memory_profiling,
      "memory_profiling",
      "This setting controls whether the memory profiling functionality of\n"
      "libprocess should be exposed when jemalloc is detected.\n"
      "NOTE: Even if set to true, memory profiling will not work unless\n"
      "jemalloc is loaded into the address space of the binary, either by\n"
      "linking against it at compile-time or using `LD_PRELOAD`.",
      false);

  add(&Flags::zk,
      "zk",
      "ZooKeeper URL (used for leader election amongst masters).\n"
      "May be one of:\n"
      "  `zk://host1:port1,host2:port2,.../path`\n"
      "  `zk://username:password@host1:port1,host2:port2,.../path`\n"
      "  `file:///path/to/file` (where file contains one of the above)\n"
      "NOTE: Not required if master is run in standalone mode (non-HA).");

  add(&Flags::ip_discovery_command,
      "ip_discovery_command",
      "Optional IP discovery binary: if set, it is expected to emit\n"
      "the IP address which the master will try to bind to.\n"
      "Cannot be used in conjunction with `--ip`.");

  add(&Flags::require_agent_domain,
      "require_agent_domain",
      "If true, only agents with a configured domain can register.\n",
      false);

  add(&Flags::publish_per_framework_metrics,
      "publish_per_framework_metrics",
      "If true, an extensive set of metrics for each active framework will\n"
      "be published. These metrics are useful for understanding cluster\n"
      "behavior, but can be overwhelming for very large numbers of\n"
      "frameworks.",
      true);

  add(&Flags::domain,
      "domain",
      "Domain that the master belongs to. Mesos currently only supports\n"
      "fault domains, which identify groups of hosts with similar failure\n"
      "characteristics. A fault domain consists of a region and a zone.\n"
      "All masters in the same Mesos cluster must be in the same region\n"
      "(they can be in different zones). This value can be specified as\n"
      "either a JSON-formatted string or a file path containing JSON.\n"
      "\n"
      "Example:\n"
      "{\n"
      "  \"fault_domain\":\n"
      "    {\n"
      "      \"region\":\n"
      "        {\n"
      "          \"name\": \"aws-us-east-1\"\n"
      "        },\n"
      "      \"zone\":\n"
      "        {\n"
      "          \"name\": \"aws-us-east-1a\"\n"
      "        }\n"
      "    }\n"
      "}",
      [](const Option<DomainInfo>& domain) -> Option<Error> {
        if (domain.isSome()) {
          // Don't let the user specify a domain without a fault
          // domain. This is allowed by the protobuf spec (for forward
          // compatibility with possible future changes), but is not a
          // useful configuration right now.
          if (!domain->has_fault_domain()) {
            return Error("`domain` must define `fault_domain`");
          }
        }
        return None();
      });

  add(&Flags::offer_constraints_re2_max_mem,
      "offer_constraints_re2_max_mem",
      "Limit on the memory usage of each RE2 regular expression in\n"
      "framework's offer constraints. If `OfferConstraints` contain a regex\n"
      "from which a RE2 object cannot be constructed without exceeding this\n"
      "limit, then framework's attempt to subscribe or update subscription\n"
      "with these `OfferConstraints` will fail.",
      DEFAULT_OFFER_CONSTRAINTS_RE2_MAX_MEM);

  add(&Flags::offer_constraints_re2_max_program_size,
      "offer_constraints_re2_max_program_size",
      "Limit on the RE2 program size of each regular expression in\n"
      "framework's offer constraints. If `OfferConstraints` contain a regex\n"
      "which results in a RE2 object exceeding this limit,\n"
      "then framework's attempt to subscribe or update subscription\n"
      "with these `OfferConstraints` will fail.",
      DEFAULT_OFFER_CONSTRAINTS_RE2_MAX_PROGRAM_SIZE);
}
