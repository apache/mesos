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

#include <stout/flags.hpp>

#include <mesos/type_utils.hpp>

#include "common/parse.hpp"
#include "master/constants.hpp"
#include "master/flags.hpp"


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
      "that the slave binds to; unless the user explicitly prevents\n"
      "that, using --no-hostname_lookup, in which case the IP itself\n"
      "is used.");

  add(&Flags::hostname_lookup,
      "hostname_lookup",
      "Whether we should execute a lookup to find out the server's hostname,\n"
      "if not explicitly set (via, e.g., `--hostname`).\n"
      "True by default; if set to 'false' it will cause Mesos\n"
      "to use the IP address, unless the hostname is explicitly set.",
      true);

  add(&Flags::root_submissions,
      "root_submissions",
      "Can root submit frameworks?",
      true);

  add(&Flags::work_dir,
      "work_dir",
      "Directory path to store the persistent information stored in the \n"
      "Registry. (example: /var/lib/mesos/master)");

  // TODO(bmahler): Consider removing 'in_memory' as it was only
  // used before 'replicated_log' was implemented.
  add(&Flags::registry,
      "registry",
      "Persistence strategy for the registry;\n"
      "available options are 'replicated_log', 'in_memory' (for testing).",
      "replicated_log");

  // TODO(vinod): Instead of specifying the quorum size consider
  // specifying the number of masters or the list of masters.
  add(&Flags::quorum,
      "quorum",
      "The size of the quorum of replicas when using 'replicated_log' based\n"
      "registry. It is imperative to set this value to be a majority of\n"
      "masters i.e., quorum > (number of masters)/2.");

  add(&Flags::zk_session_timeout,
      "zk_session_timeout",
      "ZooKeeper session timeout.",
      ZOOKEEPER_SESSION_TIMEOUT);

  // TODO(bmahler): Set the default to true in 0.20.0.
  add(&Flags::registry_strict,
      "registry_strict",
      "Whether the Master will take actions based on the persistent\n"
      "information stored in the Registry. Setting this to false means\n"
      "that the Registrar will never reject the admission, readmission,\n"
      "or removal of a slave. Consequently, 'false' can be used to\n"
      "bootstrap the persistent state on a running cluster.\n"
      "NOTE: This flag is *experimental* and should not be used in\n"
      "production yet.",
      false);

  add(&Flags::registry_fetch_timeout,
      "registry_fetch_timeout",
      "Duration of time to wait in order to fetch data from the registry\n"
      "after which the operation is considered a failure.",
      Seconds(60));

  add(&Flags::registry_store_timeout,
      "registry_store_timeout",
      "Duration of time to wait in order to store data in the registry\n"
      "after which the operation is considered a failure.",
      Seconds(5));

  add(&Flags::log_auto_initialize,
      "log_auto_initialize",
      "Whether to automatically initialize the replicated log used for the\n"
      "registry. If this is set to false, the log has to be manually\n"
      "initialized when used for the very first time.",
      true);

  add(&Flags::slave_reregister_timeout,
      "slave_reregister_timeout",
      "The timeout within which all slaves are expected to re-register\n"
      "when a new master is elected as the leader. Slaves that do not\n"
      "re-register within the timeout will be removed from the registry\n"
      "and will be shutdown if they attempt to communicate with master.\n"
      "NOTE: This value has to be atleast " +
        stringify(MIN_SLAVE_REREGISTER_TIMEOUT) + ".",
      MIN_SLAVE_REREGISTER_TIMEOUT);

  // TODO(bmahler): Add a 'Percentage' abstraction for flags.
  // TODO(bmahler): Add a --production flag for production defaults.
  add(&Flags::recovery_slave_removal_limit,
      "recovery_slave_removal_limit",
      "For failovers, limit on the percentage of slaves that can be removed\n"
      "from the registry *and* shutdown after the re-registration timeout\n"
      "elapses. If the limit is exceeded, the master will fail over rather\n"
      "than remove the slaves.\n"
      "This can be used to provide safety guarantees for production\n"
      "environments. Production environments may expect that across Master\n"
      "failovers, at most a certain percentage of slaves will fail\n"
      "permanently (e.g. due to rack-level failures).\n"
      "Setting this limit would ensure that a human needs to get\n"
      "involved should an unexpected widespread failure of slaves occur\n"
      "in the cluster.\n"
      "Values: [0%-100%]",
      stringify(RECOVERY_SLAVE_REMOVAL_PERCENT_LIMIT * 100.0) + "%");

  // TODO(vinod): Add a 'Rate' abstraction in stout and the
  // corresponding parser for flags.
  add(&Flags::slave_removal_rate_limit,
      "slave_removal_rate_limit",
      "The maximum rate (e.g., 1/10mins, 2/3hrs, etc) at which slaves will\n"
      "be removed from the master when they fail health checks. By default\n"
      "slaves will be removed as soon as they fail the health checks.\n"
      "The value is of the form <Number of slaves>/<Duration>.");

  add(&Flags::webui_dir,
      "webui_dir",
      "Directory path of the webui files/assets",
      PKGDATADIR "/webui");

  add(&Flags::whitelist,
      "whitelist",
      "Path to a file with a list of slaves\n"
      "(one per line) to advertise offers for.\n"
      "Path could be of the form 'file:///path/to/file' or '/path/to/file'.");

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
      Seconds(1));

  add(&Flags::cluster,
      "cluster",
      "Human readable name for the cluster,\n"
      "displayed in the webui.");

  // TODO(vinod): Deprecate this in favor of '--acls'.
  add(&Flags::roles,
      "roles",
      "A comma-separated list of the allocation\n"
      "roles that frameworks in this cluster may\n"
      "belong to.");

  add(&Flags::weights,
      "weights",
      "A comma-separated list of role/weight pairs\n"
      "of the form 'role=weight,role=weight'. Weights\n"
      "are used to indicate forms of priority.");

  // TODO(adam-mesos): Deprecate --authenticate for --authenticate_frameworks.
  add(&Flags::authenticate_frameworks,
      "authenticate",
      "If authenticate is 'true' only authenticated frameworks are allowed\n"
      "to register. If 'false' unauthenticated frameworks are also\n"
      "allowed to register.",
      false);

  add(&Flags::authenticate_slaves,
      "authenticate_slaves",
      "If 'true' only authenticated slaves are allowed to register.\n"
      "If 'false' unauthenticated slaves are also allowed to register.",
      false);

  add(&Flags::credentials,
      "credentials",
      "Either a path to a text file with a list of credentials,\n"
      "each line containing 'principal' and 'secret' separated by "
      "whitespace,\n"
      "or, a path to a JSON-formatted file containing credentials.\n"
      "Path could be of the form 'file:///path/to/file' or '/path/to/file'."
      "\n"
      "JSON file Example:\n"
      "{\n"
      "  \"credentials\": [\n"
      "                    {\n"
      "                       \"principal\": \"sherman\",\n"
      "                       \"secret\": \"kitesurf\",\n"
      "                    }\n"
      "                   ]\n"
      "}\n"
      "Text file Example:\n"
      "username secret");

  add(&Flags::acls,
      "acls",
      "The value could be a JSON-formatted string of ACLs\n"
      "or a file path containing the JSON-formatted ACLs used\n"
      "for authorization. Path could be of the form 'file:///path/to/file'\n"
      "or '/path/to/file'.\n"
      "\n"
      "Note that if the flag --authorizers is provided with a value different\n"
      "than '" + DEFAULT_AUTHORIZER + "', the ACLs contents will be ignored.\n"
      "\n"
      "See the ACLs protobuf in mesos.proto for the expected format.\n"
      "\n"
      "Example:\n"
      "{\n"
      "  \"register_frameworks\": [\n"
      "                       {\n"
      "                          \"principals\": { \"type\": \"ANY\" },\n"
      "                          \"roles\": { \"values\": [\"a\"] }\n"
      "                       }\n"
      "                     ],\n"
      "  \"run_tasks\": [\n"
      "                  {\n"
      "                     \"principals\": { \"values\": [\"a\", \"b\"] },\n"
      "                     \"users\": { \"values\": [\"c\"] }\n"
      "                  }\n"
      "                ],\n"
      "  \"shutdown_frameworks\": [\n"
      "                {\n"
      "                   \"principals\": { \"values\": [\"a\", \"b\"] },\n"
      "                   \"framework_principals\": { \"values\": [\"c\"] }\n"
      "                }\n"
      "              ]\n"
      "}");

  add(&Flags::firewall_rules,
      "firewall_rules",
      "The value could be a JSON-formatted string of rules or a\n"
      "file path containing the JSON-formatted rules used in the endpoints\n"
      "firewall. Path must be of the form 'file:///path/to/file'\n"
      "or '/path/to/file'.\n"
      "\n"
      "See the Firewall message in flags.proto for the expected format.\n"
      "\n"
      "Example:\n"
      "{\n"
      "  \"disabled_endpoints\" : {\n"
      "    \"paths\" : [\n"
      "      \"/files/browse\",\n"
      "      \"/slave(0)/stats.json\",\n"
      "    ]\n"
      "  }\n"
      "}");

  add(&Flags::rate_limits,
      "rate_limits",
      "The value could be a JSON-formatted string of rate limits\n"
      "or a file path containing the JSON-formatted rate limits used\n"
      "for framework rate limiting.\n"
      "Path could be of the form 'file:///path/to/file'\n"
      "or '/path/to/file'.\n"
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
  add(&Flags::max_executors_per_slave,
      "max_executors_per_slave",
      "Maximum number of executors allowed per slave. The network\n"
      "monitoring/isolation technique imposes an implicit resource\n"
      "acquisition on each executor (# ephemeral ports), as a result\n"
      "one can only run a certain number of executors on each slave.");
#endif // WITH_NETWORK_ISOLATOR

  // TODO(karya): When we have optimistic offers, this will only
  // benefit frameworks that accidentally lose an offer.
  add(&Flags::offer_timeout,
      "offer_timeout",
      "Duration of time before an offer is rescinded from a framework.\n"
      "This helps fairness when running frameworks that hold on to offers,\n"
      "or frameworks that accidentally drop offers.");

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
      "file containing a JSON-formatted string. 'filepath' can be\n"
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

  add(&Flags::authenticators,
      "authenticators",
      "Authenticator implementation to use when authenticating frameworks\n"
      "and/or slaves. Use the default '" + DEFAULT_AUTHENTICATOR + "', or\n"
      "load an alternate authenticator module using --modules.",
      DEFAULT_AUTHENTICATOR);

  add(&Flags::allocator,
      "allocator",
      "Allocator to use for resource allocation to frameworks.\n"
      "Use the default '" + DEFAULT_ALLOCATOR + "' allocator, or\n"
      "load an alternate allocator module using --modules.",
      DEFAULT_ALLOCATOR);

  add(&Flags::hooks,
      "hooks",
      "A comma-separated list of hook modules to be\n"
      "installed inside master.");

  add(&Flags::slave_ping_timeout,
      "slave_ping_timeout",
      "The timeout within which each slave is expected to respond to a\n"
      "ping from the master. Slaves that do not respond within\n"
      "max_slave_ping_timeouts ping retries will be asked to shutdown.\n"
      "NOTE: The total ping timeout (slave_ping_timeout multiplied by\n"
      "max_slave_ping_timeouts) should be greater than the ZooKeeper\n"
      "session timeout to prevent useless re-registration attempts.\n",
      DEFAULT_SLAVE_PING_TIMEOUT,
      [](const Duration& value) -> Option<Error> {
        if (value < Seconds(1) || value > Minutes(15)) {
          return Error("Expected --slave_ping_timeout to be between " +
                       stringify(Seconds(1)) + " and " +
                       stringify(Minutes(15)));
        }
        return None();
      });

  add(&Flags::max_slave_ping_timeouts,
      "max_slave_ping_timeouts",
      "The number of times a slave can fail to respond to a\n"
      "ping from the master. Slaves that do not respond within\n"
      "max_slave_ping_timeouts ping retries will be asked to shutdown.\n",
      DEFAULT_MAX_SLAVE_PING_TIMEOUTS,
      [](size_t value) -> Option<Error> {
        if (value < 1) {
          return Error("Expected --max_slave_ping_timeouts to be at least 1");
        }
        return None();
      });


  add(&Flags::authorizers,
      "authorizers",
      "Authorizer implementation to use when authorizating actions that\n"
      "required it.\n"
      "Use the default '" + DEFAULT_AUTHORIZER + "', or\n"
      "load an alternate authorizer module using --modules.\n"
      "\n"
      "Note that if the flag --authorizers is provided with a value different\n"
      "than the default '" + DEFAULT_AUTHORIZER + "', the ACLs passed\n"
      "through the --acls flag will be ignored.\n"
      "\n"
      "Currently there's no support for multiple authorizers.",
      DEFAULT_AUTHORIZER);

  add(&Flags::max_completed_frameworks,
      "max_completed_frameworks",
      "Maximum number of completed frameworks to store in memory.",
      DEFAULT_MAX_COMPLETED_FRAMEWORKS);

  add(&Flags::max_completed_tasks_per_framework,
      "max_completed_tasks_per_framework",
      "Maximum number of completed tasks per framework to store in memory.",
      DEFAULT_MAX_COMPLETED_TASKS_PER_FRAMEWORK);
}
