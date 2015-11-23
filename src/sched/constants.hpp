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
extern const Duration DEFAULT_REGISTRATION_BACKOFF_FACTOR;

// The maximum interval the scheduler driver waits before retrying
// registration.
extern const Duration REGISTRATION_RETRY_INTERVAL_MAX;

// Name of the default, CRAM-MD5 authenticatee.
extern const std::string DEFAULT_AUTHENTICATEE;

} // namespace scheduler {
} // namespace internal {
} // namespace mesos {

#endif // __SCHED_CONSTANTS_HPP__
