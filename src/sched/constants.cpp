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

#include "sched/constants.hpp"

namespace mesos {
namespace internal {
namespace scheduler {

// NOTE: The default backoff factor for the scheduler (2s) is
// different from the slave (1s) because the scheduler driver doesn't
// do an initial backoff for the very first attempt unlike the slave.
// TODO(vinod): Once we fix the scheduler driver to do initial backoff
// we can change the default to 1s.
const Duration REGISTRATION_BACKOFF_FACTOR = Seconds(2);

const Duration REGISTRATION_RETRY_INTERVAL_MAX = Minutes(1);

const std::string DEFAULT_AUTHENTICATEE = "crammd5";

} // namespace scheduler {
} // namespace internal {
} // namespace mesos {
