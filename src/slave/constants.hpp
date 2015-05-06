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

#ifndef __SLAVE_CONSTANTS_HPP__
#define __SLAVE_CONSTANTS_HPP__

#include <stdint.h>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>

namespace mesos {
namespace internal {
namespace slave {

// TODO(bmahler): It appears there may be a bug with gcc-4.1.2 in which these
// duration constants were not being initialized when having static linkage.
// This issue did not manifest in newer gcc's. Specifically, 4.2.1 was ok.
// So we've moved these to have external linkage but perhaps in the future
// we can revert this.

// TODO(jieyu): Use static functions for all the constants. See more
// details in MESOS-1023.

extern const Duration EXECUTOR_REGISTRATION_TIMEOUT;
extern const Duration EXECUTOR_SHUTDOWN_GRACE_PERIOD;
extern const Duration EXECUTOR_REREGISTER_TIMEOUT;
extern const Duration EXECUTOR_SIGNAL_ESCALATION_TIMEOUT;
extern const Duration RECOVERY_TIMEOUT;
extern const Duration STATUS_UPDATE_RETRY_INTERVAL_MIN;
extern const Duration STATUS_UPDATE_RETRY_INTERVAL_MAX;
extern const Duration GC_DELAY;
extern const Duration DISK_WATCH_INTERVAL;
extern const Duration RESOURCE_MONITORING_INTERVAL;

// Default backoff interval used by the slave to wait before registration.
extern const Duration REGISTRATION_BACKOFF_FACTOR;

// The maximum interval the slave waits before retrying registration.
// Note that this value has to be << 'MIN_SLAVE_REREGISTER_TIMEOUT'
// declared in 'master/constants.hpp'. This helps the slave to retry
// (re-)registration multiple times between when the master finishes
// recovery and when it times out slave re-registration.
extern const Duration REGISTER_RETRY_INTERVAL_MAX;

// Minimum free disk capacity enforced by the garbage collector.
extern const double GC_DISK_HEADROOM;

// Maximum number of completed frameworks to store in memory.
extern const uint32_t MAX_COMPLETED_FRAMEWORKS;

// Maximum number of completed executors per framework to store in memory.
extern const uint32_t MAX_COMPLETED_EXECUTORS_PER_FRAMEWORK;

// Maximum number of completed tasks per executor to store in memory.
extern const uint32_t MAX_COMPLETED_TASKS_PER_EXECUTOR;

// Default cpus offered by the slave.
extern const double DEFAULT_CPUS;

// Default memory offered by the slave.
extern const Bytes DEFAULT_MEM;

// Default disk space offered by the slave.
extern const Bytes DEFAULT_DISK;

// Default ports range offered by the slave.
extern const std::string DEFAULT_PORTS;

// Default cpu resource given to a command executor.
const double DEFAULT_EXECUTOR_CPUS = 0.1;

// Default memory resource given to a command executor.
const Bytes DEFAULT_EXECUTOR_MEM = Megabytes(32);

#ifdef WITH_NETWORK_ISOLATOR
// Default number of ephemeral ports allocated to a container by the
// network isolator.
extern const uint16_t DEFAULT_EPHEMERAL_PORTS_PER_CONTAINER;
#endif

// Default duration that docker containers will be removed after exit.
extern const Duration DOCKER_REMOVE_DELAY;

// Name of the default, CRAM-MD5 authenticatee.
extern const std::string DEFAULT_AUTHENTICATEE;

// If no pings received within this timeout, then the slave will
// trigger a re-detection of the master to cause a re-registration.
Duration MASTER_PING_TIMEOUT();


// To avoid overwhelming the master, we enforce a minimal delay
// between two subsequent UpdateOversubscribedResourcesMessages.
Duration UPDATE_OVERSUBSCRIBED_RESOURCES_INTERVAL_MIN();

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CONSTANTS_HPP__
