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

#ifndef __SCHED_FLAGS_HPP__
#define __SCHED_FLAGS_HPP__

#include <stout/flags.hpp>

#include "logging/flags.hpp"

#include "sched/constants.hpp"

namespace mesos {
namespace internal {
namespace scheduler {

class Flags : public logging::Flags
{
public:
  Flags()
  {
    add(&Flags::registration_backoff_factor,
        "registration_backoff_factor",
        "Scheduler driver (re-)registration retries are exponentially backed\n"
        "off based on 'b', the registration backoff factor (e.g., 1st retry\n"
        "uses a random value between [0, b], 2nd retry between [0, b * 2^1],\n"
        "3rd retry between [0, b * 2^2]...) up to a maximum of (framework\n"
        "failover timeout/10, if failover timeout is specified) or " +
        stringify(REGISTRATION_RETRY_INTERVAL_MAX) + ", whichever is smaller",
        REGISTRATION_BACKOFF_FACTOR);
  }

  Duration registration_backoff_factor;
};

} // namespace scheduler {
} // namespace internal {
} // namespace mesos {

#endif // __SCHED_FLAGS_HPP__
