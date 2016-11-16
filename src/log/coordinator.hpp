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

#ifndef __LOG_COORDINATOR_HPP__
#define __LOG_COORDINATOR_HPP__

#include <stdint.h>

#include <string>

#include <process/future.hpp>
#include <process/shared.hpp>

#include <stout/option.hpp>

#include "log/network.hpp"
#include "log/replica.hpp"

namespace mesos {
namespace internal {
namespace log {

// Forward declaration.
class CoordinatorProcess;


class Coordinator
{
public:
  Coordinator(
      size_t quorum,
      const process::Shared<Replica>& replica,
      const process::Shared<Network>& network);

  ~Coordinator();

  // Handles coordinator election. Returns the last committed (a.k.a.,
  // learned) log position if the operation succeeds. Returns none if
  // the election is not successful, but can be retried.
  process::Future<Option<uint64_t>> elect();

  // Handles coordinator demotion. Returns the last committed (a.k.a.,
  // learned) log position if the operation succeeds. One should only
  // call this function if the coordinator has been elected, and no
  // write (append or truncate) is in progress.
  process::Future<uint64_t> demote();

  // Appends the specified bytes to the end of the log. Returns the
  // position of the appended entry if the operation succeeds or none
  // if the coordinator was demoted.
  process::Future<Option<uint64_t>> append(const std::string& bytes);

  // Removes all log entries preceding the log entry at the given
  // position (to). Returns the position at which the truncate
  // operation is written if the operation succeeds or none if the
  // coordinator was demoted.
  process::Future<Option<uint64_t>> truncate(uint64_t to);

private:
  CoordinatorProcess* process;
};

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_COORDINATOR_HPP__
