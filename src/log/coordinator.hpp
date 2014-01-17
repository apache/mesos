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

#ifndef __LOG_COORDINATOR_HPP__
#define __LOG_COORDINATOR_HPP__

#include <stdint.h>

#include <string>

#include <process/shared.hpp>
#include <process/timeout.hpp>

#include <stout/result.hpp>

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
      size_t _quorum,
      const process::Shared<Replica>& _replica,
      const process::Shared<Network>& _network);

  ~Coordinator();

  // Handles coordinator election/demotion. A result of none means the
  // coordinator failed to achieve a quorum (e.g., due to timeout) but
  // can be retried. A some result returns the last committed log
  // position.
  Result<uint64_t> elect(const process::Timeout& timeout);
  Result<uint64_t> demote();

  // Returns the result of trying to append the specified bytes. A
  // result of none means the append failed (e.g., due to timeout),
  // but can be retried.
  Result<uint64_t> append(
      const std::string& bytes,
      const process::Timeout& timeout);

  // Returns the result of trying to truncate the log (from the
  // beginning to the specified position exclusive). A result of
  // none means the truncate failed (e.g., due to timeout), but can be
  // retried.
  Result<uint64_t> truncate(uint64_t to, const process::Timeout& timeout);

private:
  CoordinatorProcess* process;
};

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_COORDINATOR_HPP__
