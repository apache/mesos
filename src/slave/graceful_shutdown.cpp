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

#include "stout/duration.hpp"

#include "logging/logging.hpp"

#include "slave/constants.hpp"
#include "slave/graceful_shutdown.hpp"

namespace mesos {
namespace slave {

// Calculates the shutdown grace period (aka shutdown timeout) so it
// is bigger than the nested one. To adjust the timeout correctly, the
// caller should provide its level index in the shutdown chain.
// Timeout adjustment gives the caller process enough time to
// terminate the underlying process before the caller, in turn, is
// killed by its parent (see the sequence chart in the
// graceful_shutdown.hpp). This approach guarantees a nested timeout
// is always greater than the parent one, but not that it is
// sufficient for the graceful shutdown to happen.
Duration calculateGracePeriod(
    Duration gracePeriod,
    int callerLevel)
{
  if (gracePeriod < Duration::zero()) {
    LOG(WARNING) << "Shutdown grace period should be nonnegative (got "
                 << gracePeriod << "), using default value: "
                 << EXECUTOR_SHUTDOWN_GRACE_PERIOD;
    gracePeriod = EXECUTOR_SHUTDOWN_GRACE_PERIOD;
  }

  gracePeriod += GRACE_PERIOD_DELTA * callerLevel;

  return gracePeriod;
}


Duration getContainerizerGracePeriod(const Duration& baseShutdownTimeout)
{
  return calculateGracePeriod(baseShutdownTimeout, 2);
}


Duration getExecGracePeriod(const Duration& baseShutdownTimeout)
{
  return calculateGracePeriod(baseShutdownTimeout, 1);
}


Duration getExecutorGracePeriod(const Duration& baseShutdownTimeout)
{
  return calculateGracePeriod(baseShutdownTimeout, 0);
}


} // namespace slave {
} // namespace mesos {
