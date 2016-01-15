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

#ifndef __MASTER_CONSTANTS_HPP__
#define __MASTER_CONSTANTS_HPP__

#include <stdint.h>

#include <string>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>

namespace mesos {
namespace internal {
namespace master {

// TODO(benh): Add units after constants.
// TODO(benh): Also make configuration options be constants.

// TODO(bmahler): It appears there may be a bug with gcc-4.1.2 in which the
// duration constants were not being initialized when having static linkage.
// This issue did not manifest in newer gcc's. Specifically, 4.2.1 was ok.
// So we've moved these to have external linkage but perhaps in the future
// we can revert this.

// TODO(vinod): Move constants that are only used in flags to
// 'master/flags.hpp'.

// TODO(jieyu): Use static functions for all the constants. See more
// details in MESOS-1023.

// Maximum number of slot offers to have outstanding for each framework.
extern const int MAX_OFFERS_PER_FRAMEWORK;

// Minimum number of cpus per offer.
extern const double MIN_CPUS;

// Minimum amount of memory per offer.
extern const Bytes MIN_MEM;


// Default interval the master uses to send heartbeats to an HTTP
// scheduler.
extern const Duration DEFAULT_HEARTBEAT_INTERVAL;

// Amount of time within which a slave PING should be received.
// NOTE: The slave uses these PING constants to determine when
// the master has stopped sending pings. If these are made
// configurable, then we'll need to rely on upper/lower bounds
// to ensure that the slave is not unnecessarily triggering
// re-registrations.
extern const Duration DEFAULT_SLAVE_PING_TIMEOUT;

// Maximum number of ping timeouts until slave is considered failed.
extern const size_t DEFAULT_MAX_SLAVE_PING_TIMEOUTS;

// The minimum timeout that can be used by a newly elected leader to
// allow re-registration of slaves. Any slaves that do not re-register
// within this timeout will be shutdown.
extern const Duration MIN_SLAVE_REREGISTER_TIMEOUT;

// Default limit on the percentage of slaves that will be removed
// after recovering if no re-registration attempts were made.
// TODO(bmahler): There's no value here that works for all setups.
// Currently the default is 100% which is favorable to those running
// small clusters or experimenting with Mesos. However, it's important
// that we also prevent the catastrophic 100% removal case for
// production clusters. This TODO is to provide a --production flag
// which would allow flag defaults that are more appropriate for
// production use-cases.
extern const double RECOVERY_SLAVE_REMOVAL_PERCENT_LIMIT;

// Maximum number of removed slaves to store in the cache.
extern const size_t MAX_REMOVED_SLAVES;

// Default maximum number of completed frameworks to store in the cache.
extern const size_t DEFAULT_MAX_COMPLETED_FRAMEWORKS;

// Default maximum number of completed tasks per framework
// to store in the cache.
extern const size_t DEFAULT_MAX_COMPLETED_TASKS_PER_FRAMEWORK;

// Time interval to check for updated watchers list.
extern const Duration WHITELIST_WATCH_INTERVAL;

// Default number of tasks (limit) for /master/tasks.json endpoint.
extern const uint32_t TASK_LIMIT;

/**
 * Label used by the Leader Contender and Detector.
 *
 * \deprecated Will be deprecated as of Mesos 0.24: see MESOS-2340.
 */
extern const std::string MASTER_INFO_LABEL;

/**
 * Label used by the Leader Contender and Detector, for JSON content.
 *
 * \since Mesos 0.23 (see MESOS-2340).
 */
extern const std::string MASTER_INFO_JSON_LABEL;

// Timeout used for ZooKeeper related operations.
// TODO(vinod): Master detector/contender should use this timeout.
extern const Duration ZOOKEEPER_SESSION_TIMEOUT;

// Name of the default, CRAM-MD5 authenticator.
extern const std::string DEFAULT_AUTHENTICATOR;

// Name of the default, HierarchicalDRF authenticator.
extern const std::string DEFAULT_ALLOCATOR;

// Name of the default, local authorizer.
extern const std::string DEFAULT_AUTHORIZER;

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_CONSTANTS_HPP__
