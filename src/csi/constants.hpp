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

#ifndef __CSI_CONSTANTS_HPP__
#define __CSI_CONSTANTS_HPP__

#include <stout/duration.hpp>

namespace mesos {
namespace csi {

// The CSI volume manager initially picks a random amount of time between
// `[0, b]`, where `b = DEFAULT_RPC_RETRY_BACKOFF_FACTOR`, to retry RPC calls.
// Subsequent retries are exponentially backed off based on this interval (e.g.,
// 2nd retry uses a random value between `[0, b * 2^1]`, 3rd retry between
// `[0, b * 2^2]`, etc) up to a maximum of `DEFAULT_RPC_RETRY_INTERVAL_MAX`.
//
// TODO(chhsiao): Make the retry parameters configurable.
constexpr Duration DEFAULT_RPC_RETRY_BACKOFF_FACTOR = Seconds(10);
constexpr Duration DEFAULT_RPC_RETRY_INTERVAL_MAX = Minutes(10);

} // namespace csi {
} // namespace mesos {

#endif // __CSI_CONSTANTS_HPP__
