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

#ifndef __SCHED_CONSTANTS_HPP__
#define __SCHED_CONSTANTS_HPP__

#include <stout/duration.hpp>

namespace mesos {
namespace internal {
namespace scheduler {

// Default backoff interval used by the scheduler driver to wait
// before registration.
//
// NOTE: The default backoff factor for the scheduler (2s) is
// different from the slave (1s) because the scheduler driver doesn't
// do an initial backoff for the very first attempt unlike the slave.
//
// TODO(vinod): Once we fix the scheduler driver to do initial backoff
// we can change the default to 1s.
constexpr Duration DEFAULT_REGISTRATION_BACKOFF_FACTOR = Seconds(2);

// The maximum interval the scheduler driver waits before retrying
// registration.
constexpr Duration REGISTRATION_RETRY_INTERVAL_MAX = Minutes(1);

// Name of the default, CRAM-MD5 authenticatee.
constexpr char DEFAULT_AUTHENTICATEE[] = "crammd5";

// Default value for `--authentication_backoff_factor`. The backoff timeout
// factor used by the scheduler when authenticating with the master.
constexpr Duration DEFAULT_AUTHENTICATION_BACKOFF_FACTOR = Seconds(1);

// Default value for `--authentication_timeout_min`. The minimum amount of
// time the scheduler waits before retrying authenticating with the master.
constexpr Duration DEFAULT_AUTHENTICATION_TIMEOUT_MIN = Seconds(5);

// Default value for `--authentication_timeout_max`. The maximum amount of
// time the scheduler waits before retrying authenticating with the master.
constexpr Duration DEFAULT_AUTHENTICATION_TIMEOUT_MAX = Minutes(1);

} // namespace scheduler {
} // namespace internal {
} // namespace mesos {

#endif // __SCHED_CONSTANTS_HPP__
