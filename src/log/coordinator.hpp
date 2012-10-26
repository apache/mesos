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

#include <string>
#include <vector>

#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/result.hpp>

#include "log/network.hpp"
#include "log/replica.hpp"

#include "messages/log.hpp"


namespace mesos {
namespace internal {
namespace log {

class Coordinator
{
public:
  Coordinator(int quorum, Replica* replica, Network* group);

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
  // Helper that tries to achieve consensus of the specified action. A
  // result of none means the write failed (e.g., due to timeout), but
  // can be retried.
  Result<uint64_t> write(const Action& action, const process::Timeout& timeout);

  // Helper that handles commiting an action (i.e., writing to the
  // local replica and then sending out learned messages).
  Result<uint64_t> commit(const Action& action);

  // Helper that tries to fill a position in the log.
  Result<Action> fill(uint64_t position, const process::Timeout& timeout);

  // Helper that uses the specified protocol to broadcast a request to
  // our group and return a set of futures.
  template <typename Req, typename Res>
  std::set<process::Future<Res> > broadcast(
      const Protocol<Req, Res>& protocol,
      const Req& req);

  // Helper like broadcast, but excludes our local replica.
  template <typename Req, typename Res>
  std::set<process::Future<Res> > remotecast(
      const Protocol<Req, Res>& protocol,
      const Req& req);

  // Helper like remotecast but ignores any responses.
  template <typename M>
  void remotecast(const M& m);

  bool elected; // True if this coordinator has been elected.

  const uint32_t quorum; // Quorum size.

  Replica* replica; // Local log replica.

  Network* network; // Used to broadcast requests and messages to replicas.

  uint64_t id; // Coordinator ID.

  uint64_t index; // Last position written in the log.
};

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_COORDINATOR_HPP__
