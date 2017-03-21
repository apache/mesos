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
      "The timeout within which an agent is expected to re-register.\n"
      "Agents re-register when they become disconnected from the master\n"
      "or when a new master is elected as the leader. Agents that do not\n"
      "re-register within the timeout will be marked unreachable in the\n"
      "registry; if/when the agent re-registers with the master, any\n"
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
      "advertise offers for. The file is watched, and periodically re-read to\n"
      "refresh the agent whitelist. By default there is no whitelist / all\n"
      "machines are accepted. Path could be of the form\n"
      "`file:///path/to/file` or `/path/to/file`.\n");

  add(&Flags::user_sorter,
      "user_sorter",
      "Policy to use for allocating resources\n"
      "between users. May be one of:\n"
      "  dominant_resource_fairness (drf)",
      "drf");

  add(&Flags::framework_sorter,
      "framework_sorter",
      "Policy to use for allocating resources\n"
      "between a given user's frameworks. Options\n"
      "are the same as for user_allocator.",
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

#ifdef WITH_NETWORK_ISOLATOR
  add(&Flags::max_executors_per_agent,
      "max_executors_per_agent",
      flags::DeprecatedName("max_executors_per_slave"),
      "Maximum number of executors allowed per agent. The network\n"
      "monitoring/isolation technique imposes an implicit resource\n"
      "acquisition on each executor (# ephemeral ports), as a result\n"
      "one can only run a certain number of executors on each agent.");
#endif // WITH_NETWORK_ISOLATOR

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
      "(See --modules for more information on module manifest files)\n"
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

  add(&Flags::fair_sharing_excluded_resource_names,
      "fair_sharing_excluded_resource_names",
      "A comma-separated list of the resource names (e.g. 'gpus')\n"
      "that will be excluded from fair sharing constraints.\n"
      "This may be useful in cases where the fair sharing\n"
      "implementation currently has limitations. E.g. See the\n"
      "problem of \"scarce\" resources:\n"
      "  http://www.mail-archive.com/dev@mesos.apache.org/msg35631.html\n"
      "  https://issues.apache.org/jira/browse/MESOS-5377");

  add(&Flags::hooks,
      "hooks",
      "A comma-separated list of hook modules to be\n"
      "installed inside master.");

  add(&Flags::agent_ping_timeout,
      "agent_ping_timeout",
      flags::DeprecatedName("slave_ping_timeout"),
      "The timeout within which an agent is expected to respond to a\n"
      "ping from the master. Agents that do not respond within\n"
      "max_agent_ping_timeouts ping retries will be asked to shutdown.\n"
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
      "`max_agent_ping_timeouts` ping retries will be asked to shutdown.\n",
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
      "Currently there's no support for multiple authorizers.",
      DEFAULT_AUTHORIZER);

  add(&Flags::http_authenticators,
      "http_authenticators",
      "HTTP authenticator implementation to use when handling requests to\n"
      "authenticated endpoints. Use the default\n"
      "`" + string(DEFAULT_HTTP_AUTHENTICATOR) + "`, or load an alternate\n"
      "HTTP authenticator module using `--modules`.\n"
      "\n"
      "Currently there is no support for multiple HTTP authenticators.",
      DEFAULT_HTTP_AUTHENTICATOR);

  add(&Flags::http_framework_authenticators,
      "http_framework_authenticators",
      "HTTP authenticator implementation to use when authenticating HTTP\n"
      "frameworks. Use the \n"
      "`" + string(DEFAULT_HTTP_AUTHENTICATOR) + "` authenticator or load an\n"
      "alternate authenticator module using `--modules`.\n"
      "Must be used in conjunction with `--http_authenticate_frameworks`.\n"
      "\n"
      "Currently there is no support for multiple HTTP framework\n"
      "authenticators.");

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
      "and removed agents. Note that the registry always stores information\n"
      "on all connected agents. If there are more than\n"
      "`registry_max_agent_count` partitioned or removed agents, agent\n"
      "information may be discarded from the registry sooner than indicated\n"
      "by this parameter.",
      DEFAULT_REGISTRY_MAX_AGENT_AGE);

  add(&Flags::registry_max_agent_count,
      "registry_max_agent_count",
      "Maximum number of disconnected agents to store in the registry.\n"
      "This informtion allows frameworks to determine the status of\n"
      "disconnected agents. Note that the registry always stores\n"
      "information about all connected agents. See also the\n"
      "`registry_max_agent_age` flag.",
      DEFAULT_REGISTRY_MAX_AGENT_COUNT);
}
