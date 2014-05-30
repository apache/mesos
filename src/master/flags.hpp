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

#ifndef __MASTER_FLAGS_HPP__
#define __MASTER_FLAGS_HPP__

#include <string>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>

#include "logging/flags.hpp"

#include "master/constants.hpp"

namespace mesos {
namespace internal {
namespace master {

class Flags : public logging::Flags
{
public:
  Flags()
  {
    add(&Flags::version,
        "version",
        "Show version and exit.",
        false);

    add(&Flags::hostname,
        "hostname",
        "The hostname the master should advertise in ZooKeeper.\n"
        "If left unset, system hostname will be used (recommended).");

    add(&Flags::root_submissions,
        "root_submissions",
        "Can root submit frameworks?",
        true);

    add(&Flags::work_dir,
        "work_dir",
        "Where to store the persistent information stored in the Registry.");

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

    add(&Flags::webui_dir,
        "webui_dir",
        "Location of the webui files/assets",
        PKGDATADIR "/webui");

    add(&Flags::whitelist,
        "whitelist",
        "Path to a file with a list of slaves\n"
        "(one per line) to advertise offers for.\n"
        "Path could be of the form 'file:///path/to/file' or '/path/to/file'.",
        "*");

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

    add(&Flags::roles,
        "roles",
        "A comma seperated list of the allocation\n"
        "roles that frameworks in this cluster may\n"
        "belong to.");

    add(&Flags::weights,
        "weights",
        "A comma seperated list of role/weight pairs\n"
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
        "Path to a file with a list of credentials.\n"
        "Each line contains 'principal' and 'secret' separated by whitespace.\n"
        "Path could be of the form 'file:///path/to/file' or '/path/to/file'.");

    // TODO(vinod): Expose this flag once the authorization feature is
    // code complete.
//  add(&Flags::acls,
//      "acls",
//      "The value could be a JSON formatted string of ACLs\n"
//      "or a file path containing the JSON formatted ACLs used\n"
//      "for authorization. Path could be of the form 'file:///path/to/file'\n"
//      "or '/path/to/file'.\n"
//      "\n"
//      "See the ACL protobuf in mesos.proto for the expected format.\n"
//      "\n"
//      "Example:\n"
//      "{\n"
//      "  \"run_tasks\": [\n"
//      "                  {\n"
//      "                     \"principals\": { values: [\"foo\", \"bar\"] },\n"
//      "                     \"users\": { values: [\"root\"] }\n"
//      "                  },\n"
//      "                  {\n"
//      "                     \"principals\": { type: \"ANY\" },\n"
//      "                     \"users\": { values: [\"guest\"] }\n"
//      "                  }\n"
//      "                ]\n"
//      "}");
  }

  bool version;
  Option<std::string> hostname;
  bool root_submissions;
  Option<std::string> work_dir;
  std::string registry;
  Option<int> quorum;
  Duration zk_session_timeout;
  bool registry_strict;
  Duration registry_fetch_timeout;
  Duration registry_store_timeout;
  bool log_auto_initialize;
  Duration slave_reregister_timeout;
  std::string recovery_slave_removal_limit;
  std::string webui_dir;
  std::string whitelist;
  std::string user_sorter;
  std::string framework_sorter;
  Duration allocation_interval;
  Option<std::string> cluster;
  Option<std::string> roles;
  Option<std::string> weights;
  bool authenticate_frameworks;
  bool authenticate_slaves;
  Option<std::string> credentials;
  Option<JSON::Object> acls;
};

} // namespace mesos {
} // namespace internal {
} // namespace master {

#endif // __MASTER_FLAGS_HPP__
