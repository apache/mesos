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

#ifndef __SLAVE_CONSTANTS_HPP__
#define __SLAVE_CONSTANTS_HPP__

#include <stdint.h>
#include <vector>

#include <mesos/mesos.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>

namespace mesos {
namespace internal {
namespace slave {

// TODO(jieyu): Use static functions for all the constants. See more
// details in MESOS-1023.

constexpr Duration EXECUTOR_REGISTRATION_TIMEOUT = Minutes(1);
constexpr Duration EXECUTOR_REREGISTRATION_TIMEOUT = Seconds(2);

// The maximum timeout within which an executor can reregister.
// Note that this value has to be << 'MIN_AGENT_REREGISTER_TIMEOUT'
// declared in 'master/constants.hpp'; since agent recovery will only
// complete after this timeout has elapsed, this ensures that the
// agent can reregister with the master before it is marked
// unreachable and its tasks are transitioned to TASK_UNREACHABLE or
// TASK_LOST.
constexpr Duration MAX_EXECUTOR_REREGISTRATION_TIMEOUT = Seconds(15);

// The default amount of time to wait for the executor to
// shut down before destroying the container.
constexpr Duration DEFAULT_EXECUTOR_SHUTDOWN_GRACE_PERIOD = Seconds(5);

// The default amount of time between heartbeats sent to HTTP executors.
constexpr Duration DEFAULT_EXECUTOR_HEARTBEAT_INTERVAL = Minutes(30);

constexpr Duration RECOVERY_TIMEOUT = Minutes(15);

// TODO(gkleiman): Move this to a different file once `TaskStatusUpdateManager`
// uses `StatusUpdateManagerProcess`. See MESOS-8296.
constexpr Duration STATUS_UPDATE_RETRY_INTERVAL_MIN = Seconds(10);
constexpr Duration STATUS_UPDATE_RETRY_INTERVAL_MAX = Minutes(10);

// Default backoff interval used by the slave to wait before registration.
constexpr Duration DEFAULT_REGISTRATION_BACKOFF_FACTOR = Seconds(1);

// The maximum interval the slave waits before retrying registration.
// Note that this value has to be << 'MIN_SLAVE_REREGISTER_TIMEOUT'
// declared in 'master/constants.hpp'. This helps the slave to retry
// (re-)registration multiple times between when the master finishes
// recovery and when it times out slave re-registration.
constexpr Duration REGISTER_RETRY_INTERVAL_MAX = Minutes(1);

// Default value for `--authentication_backoff_factor`. The backoff interval
// factor affects the agent timeout interval after failed authentications.
constexpr Duration DEFAULT_AUTHENTICATION_BACKOFF_FACTOR = Seconds(1);

// Default value for `--authentication_timeout_min`. The minimum timeout
// interval the agent waits before retrying authentication.
constexpr Duration DEFAULT_AUTHENTICATION_TIMEOUT_MIN = Seconds(5);

// Default value for `--authentication_timeout_max`. The maximum timeout
// interval the agent waits before retrying authentication.
constexpr Duration DEFAULT_AUTHENTICATION_TIMEOUT_MAX = Minutes(1);

constexpr Duration GC_DELAY = Weeks(1);
constexpr Duration DISK_WATCH_INTERVAL = Minutes(1);

// Minimum free disk capacity enforced by the garbage collector.
constexpr double GC_DISK_HEADROOM = 0.1;

// Maximum number of completed frameworks to store in memory.
constexpr size_t MAX_COMPLETED_FRAMEWORKS = 50;

// Default maximum number of completed executors per framework
// to store in memory.
constexpr size_t DEFAULT_MAX_COMPLETED_EXECUTORS_PER_FRAMEWORK = 150;

// Maximum number of a container id length, according to the
// max entry length for directory names on AUFS.
constexpr size_t MAX_CONTAINER_ID_LENGTH = 242;

// Maximum number of completed tasks per executor to store in memory.
//
// NOTE: This should be greater than zero because the agent looks
// for completed tasks to determine (with false positives) whether
// an executor ever received tasks. See MESOS-8411.
//
// TODO(mzhu): Remove this note once we can determine whether an
// executor ever received tasks without looking through the
// completed tasks.
constexpr size_t MAX_COMPLETED_TASKS_PER_EXECUTOR = 200;

// Default cpus offered by the slave.
constexpr double DEFAULT_CPUS = 1;

// Default memory offered by the slave.
constexpr Bytes DEFAULT_MEM = Gigabytes(1);

// Default disk space offered by the slave.
constexpr Bytes DEFAULT_DISK = Gigabytes(10);

// Default ports range offered by the slave.
constexpr char DEFAULT_PORTS[] = "[31000-32000]";

// Default cpu resource given to a command executor.
constexpr double DEFAULT_EXECUTOR_CPUS = 0.1;

// Default memory resource given to a command executor.
constexpr Bytes DEFAULT_EXECUTOR_MEM = Megabytes(32);

#ifdef ENABLE_PORT_MAPPING_ISOLATOR
// Default number of ephemeral ports allocated to a container by the
// network isolator.
constexpr uint16_t DEFAULT_EPHEMERAL_PORTS_PER_CONTAINER = 1024;
#endif

// Default UNIX socket (Linux) or Named Pipe (Windows) resource that provides
// CLI access to the Docker daemon.
#ifdef __WINDOWS__
constexpr char DEFAULT_DOCKER_HOST_RESOURCE[] = "//./pipe/docker_engine";
#else
constexpr char DEFAULT_DOCKER_HOST_RESOURCE[] = "/var/run/docker.sock";
#endif // __WINDOWS__

// Default filename used for domain socket-based communication between
// agent and executors, if that is enabled.
constexpr char AGENT_EXECUTORS_SOCKET_FILENAME[] = "agent.sock";

// Default duration that docker containers will be removed after exit.
constexpr Duration DOCKER_REMOVE_DELAY = Hours(6);

// Default duration to wait before retry inspecting a docker
// container.
constexpr Duration DOCKER_INSPECT_DELAY = Seconds(1);

// Default duration to wait for `inspect` command completion.
constexpr Duration DOCKER_INSPECT_TIMEOUT = Seconds(5);

// Default maximum number of docker inspect calls docker ps will invoke
// in parallel to prevent hitting system's open file descriptor limit.
constexpr size_t DOCKER_PS_MAX_INSPECT_CALLS = 100;

// Default duration that docker containerizer will wait to check
// docker version.
// TODO(tnachen): Make this a flag.
constexpr Duration DOCKER_VERSION_WAIT_TIMEOUT = Seconds(5);

// Additional duration that docker containerizer will wait beyond the
// configured `docker_stop_timeout` for docker stop to succeed, before
// trying to kill the process by itself.
constexpr Duration DOCKER_FORCE_KILL_TIMEOUT = Seconds(1);

// Name of the default, CRAM-MD5 authenticatee.
constexpr char DEFAULT_AUTHENTICATEE[] = "crammd5";

// Name of the default, local authorizer.
constexpr char DEFAULT_AUTHORIZER[] = "local";

// Name of the agent HTTP authentication realm for read-only endpoints.
constexpr char READONLY_HTTP_AUTHENTICATION_REALM[] = "mesos-agent-readonly";

// Name of the agent HTTP authentication realm for read-write endpoints.
constexpr char READWRITE_HTTP_AUTHENTICATION_REALM[] = "mesos-agent-readwrite";

// Name of the agent HTTP authentication realm for HTTP executors.
constexpr char EXECUTOR_HTTP_AUTHENTICATION_REALM[] = "mesos-agent-executor";

// Name of the agent HTTP authentication realm for HTTP resource providers.
constexpr char RESOURCE_PROVIDER_HTTP_AUTHENTICATION_REALM[] =
  "mesos-agent-resource-provider";

// Default maximum storage space to be used by the fetcher cache.
constexpr Bytes DEFAULT_FETCHER_CACHE_SIZE = Gigabytes(2);

// Default timeout for the fetcher to wait when a net download stalls.
constexpr Duration DEFAULT_FETCHER_STALL_TIMEOUT = Minutes(1);

// If no pings received within this timeout, then the slave will
// trigger a re-detection of the master to cause a re-registration.
Duration DEFAULT_MASTER_PING_TIMEOUT();

// Name of the executable for default executor.
#ifdef __WINDOWS__
constexpr char MESOS_DEFAULT_EXECUTOR[] = "mesos-default-executor.exe";
constexpr char MESOS_EXECUTOR[] = "mesos-executor.exe";
#else
constexpr char MESOS_DEFAULT_EXECUTOR[] = "mesos-default-executor";
constexpr char MESOS_EXECUTOR[] = "mesos-executor";
#endif // __WINDOWS__

// Name of the component used for describing pending futures.
constexpr char COMPONENT_NAME_CONTAINERIZER[] = "containerizer";


// Virtual path on which agent logs are mounted in `/files/` endpoint.
constexpr char AGENT_LOG_VIRTUAL_PATH[] = "/slave/log";

std::vector<SlaveInfo::Capability> AGENT_CAPABILITIES();

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CONSTANTS_HPP__
