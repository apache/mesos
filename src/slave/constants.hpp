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

#include <stout/duration.hpp>

namespace mesos {
namespace internal {
namespace slave {

const Duration EXECUTOR_SHUTDOWN_GRACE_PERIOD = Seconds(5.0);
const Duration STATUS_UPDATE_RETRY_INTERVAL = Seconds(10.0);
const Duration GC_DELAY = Weeks(1.0);
const Duration DISK_WATCH_INTERVAL = Minutes(1.0);

// Maximum number of completed frameworks to store in memory.
const uint32_t MAX_COMPLETED_FRAMEWORKS = 50;

// Maximum number of completed executors per framework to store in memory.
const uint32_t MAX_COMPLETED_EXECUTORS_PER_FRAMEWORK = 150;

// Maximum number of completed tasks per executor to store in memeory.
const uint32_t MAX_COMPLETED_TASKS_PER_EXECUTOR = 200;

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CONSTANTS_HPP__
