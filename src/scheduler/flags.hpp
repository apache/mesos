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

#ifndef __SCHEDULER_FLAGS_HPP__
#define __SCHEDULER_FLAGS_HPP__

#include "local/flags.hpp"

#include "scheduler/constants.hpp"

namespace mesos {
namespace v1 {
namespace scheduler {

class Flags : public virtual mesos::internal::local::Flags
{
public:
  Flags()
  {
    add(&Flags::connectionDelayMax,
        "connection_delay_max",
        "The maximum amount of time to wait before trying to initiate a "
        "connection with the master. The library waits for a random amount of "
        "time between [0, b], where `b = connection_delay_max` before "
        "initiating a (re-)connection attempt with the master",
        DEFAULT_CONNECTION_DELAY_MAX);
  }

  Duration connectionDelayMax;
};

} // namespace scheduler {
} // namespace v1 {
} // namespace mesos {

#endif // __SCHEDULER_FLAGS_HPP__
