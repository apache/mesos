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

#ifndef __MASTER_CONSTANTS_HPP__
#define __MASTER_CONSTANTS_HPP__

#include <stdint.h>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>

namespace mesos {
namespace internal {
namespace master {

// TODO(benh): Add units after constants.
// TODO(benh): Also make configuration options be constants.

// TODO(vinod): Move constants that are only used in flags to
// 'master/flags.hpp'.

// TODO(jieyu): Use static functions for all the constants. See more
// details in MESOS-1023.

// Maximum number of slot offers to have outstanding for each framework.
constexpr int MAX_OFFERS_PER_FRAMEWORK = 50;

// Minimum number of cpus per offer.
constexpr double MIN_CPUS = 0.01;

// Minimum amount of memory per offer.
constexpr Bytes MIN_MEM = Megabytes(32);

// Default interval the master uses to send heartbeats to an HTTP
// scheduler.
constexpr Duration DEFAULT_HEARTBEAT_INTERVAL = Seconds(15);

// Amount of time within which a slave PING should be received.
// NOTE: The slave uses these PING constants to determine when
// the master has stopped sending pings. If these are made
// configurable, then we'll need to rely on upper/lower bounds
// to ensure that the slave is not unnecessarily triggering
// re-registrations.
constexpr Duration DEFAULT_AGENT_PING_TIMEOUT = Seconds(15);

// Maximum number of ping timeouts until slave is considered failed.
constexpr size_t DEFAULT_MAX_AGENT_PING_TIMEOUTS = 5;

// The minimum timeout that can be used by a newly elected leader to
// allow re-registration of slaves. Any slaves that do not re-register
// within this timeout will be marked unreachable; if/when the agent
// re-registers, non-partition-aware tasks running on the agent will
// be terminated.
constexpr Duration MIN_AGENT_REREGISTER_TIMEOUT = Minutes(10);

// Default limit on the percentage of slaves that will be removed
// after recovering if no re-registration attempts were made.
// TODO(bmahler): There's no value here that works for all setups.
// Currently the default is 100% which is favorable to those running
// small clusters or experimenting with Mesos. However, it's important
// that we also prevent the catastrophic 100% removal case for
// production clusters. This TODO is to provide a --production flag
// which would allow flag defaults that are more appropriate for
// production use-cases.
constexpr double RECOVERY_AGENT_REMOVAL_PERCENT_LIMIT = 1.0; // 100%.

// Maximum number of removed slaves to store in the cache.
constexpr size_t MAX_REMOVED_SLAVES = 100000;

// Default maximum number of completed frameworks to store in the cache.
constexpr size_t DEFAULT_MAX_COMPLETED_FRAMEWORKS = 50;

// Default maximum number of completed tasks per framework
// to store in the cache.
constexpr size_t DEFAULT_MAX_COMPLETED_TASKS_PER_FRAMEWORK = 1000;

// Default maximum number of unreachable tasks per framework
// to store in the cache.
constexpr size_t DEFAULT_MAX_UNREACHABLE_TASKS_PER_FRAMEWORK = 1000;

// Time interval to check for updated watchers list.
constexpr Duration WHITELIST_WATCH_INTERVAL = Seconds(5);

// Default number of tasks (limit) for /master/tasks endpoint.
constexpr size_t TASK_LIMIT = 100;

constexpr Duration DEFAULT_REGISTRY_GC_INTERVAL = Minutes(15);

constexpr Duration DEFAULT_REGISTRY_MAX_AGENT_AGE = Weeks(2);

constexpr size_t DEFAULT_REGISTRY_MAX_AGENT_COUNT = 100 * 1024;

/**
 * Label used by the Leader Contender and Detector.
 *
 * \deprecated Will be deprecated as of Mesos 0.24: see MESOS-2340.
 */
constexpr char MASTER_INFO_LABEL[] = "info";

/**
 * Label used by the Leader Contender and Detector, for JSON content.
 *
 * \since Mesos 0.23 (see MESOS-2340).
 */
constexpr char MASTER_INFO_JSON_LABEL[] = "json.info";

// Timeout used for ZooKeeper related operations.
// TODO(vinod): Master detector/contender should use this timeout.
constexpr Duration ZOOKEEPER_SESSION_TIMEOUT = Seconds(10);

// Name of the default, CRAM-MD5 authenticator.
constexpr char DEFAULT_AUTHENTICATOR[] = "crammd5";

// Name of the default, HierarchicalDRF authenticator.
constexpr char DEFAULT_ALLOCATOR[] = "HierarchicalDRF";

// The default interval between allocations.
constexpr Duration DEFAULT_ALLOCATION_INTERVAL = Seconds(1);

// Name of the default, local authorizer.
constexpr char DEFAULT_AUTHORIZER[] = "local";

// Name of the master HTTP authentication realm for read-only endpoints.
constexpr char READONLY_HTTP_AUTHENTICATION_REALM[] =
  "mesos-master-readonly";

// Name of the master HTTP authentication realm for read-write endpoints.
constexpr char READWRITE_HTTP_AUTHENTICATION_REALM[] =
  "mesos-master-readwrite";

// Name of the default authentication realm for HTTP frameworks.
constexpr char DEFAULT_HTTP_FRAMEWORK_AUTHENTICATION_REALM[] =
  "mesos-master-scheduler";

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_CONSTANTS_HPP__
