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

#include <algorithm>

#include <stout/error.hpp>
#include <stout/none.hpp>

#include "log/catchup.hpp"
#include "log/consensus.hpp"
#include "log/coordinator.hpp"

#include "messages/log.hpp"

using namespace process;

using std::set;
using std::string;

namespace mesos {
namespace internal {
namespace log {

Coordinator::Coordinator(
    size_t _quorum,
    const Shared<Replica>& _replica,
    const Shared<Network>& _network)
  : quorum(_quorum),
    replica(_replica),
    network(_network),
    elected(false),
    proposal(0),
    index(0) {}


Coordinator::~Coordinator() {}


Result<uint64_t> Coordinator::elect(const Timeout& timeout)
{
  LOG(INFO) << "Coordinator attempting to get elected within "
            << timeout.remaining();

  if (elected) {
    // TODO(benh): No-op instead of error?
    return Error("Coordinator already elected");
  }

  // Get the highest known promise from our local replica.
  Future<uint64_t> promised = replica->promised();

  if (!promised.await(timeout.remaining())) {
    promised.discard();
    return None();
  } else if (promised.isFailed()) {
    return Error(promised.failure());
  }

  CHECK(promised.isReady()) << "Not expecting a discarded future!";

  proposal = std::max(proposal, promised.get()) + 1; // Try the next highest!

  // Run the implicit promise phase.
  Future<PromiseResponse> promising = log::promise(quorum, network, proposal);

  if (!promising.await(timeout.remaining())) {
    promising.discard();
    return None();
  } else if (promising.isFailed()) {
    return Error(promising.failure());
  }

  CHECK(promising.isReady()) << "Not expecting a discarded future!";

  const PromiseResponse& response = promising.get();
  if (!response.okay()) {
    // Lost an election, but can retry.
    proposal = response.proposal();
    return None();
  } else {
    LOG(INFO) << "Coordinator elected, attempting to fill missing positions";

    CHECK(response.has_position());

    index = response.position();

    // Need to "catch-up" local replica (i.e., fill in any unlearned
    // and/or missing positions) so that we can do local reads.
    // Usually we could do this lazily, however, a local learned
    // position might have been truncated, so we actually need to
    // catch-up the local replica all the way to the end of the log
    // before we can perform any up-to-date local reads.

    Future<set<uint64_t> > positions = replica->missing(0, index);

    if (!positions.await(timeout.remaining())) {
      positions.discard();
      return None();
    } else if (positions.isFailed()) {
      return Error(positions.failure());
    }

    CHECK(positions.isReady()) << "Not expecting a discarded future!";

    Future<Nothing> catching =
      log::catchup(quorum, replica, network, proposal, positions.get());

    if (!catching.await(timeout.remaining())) {
      catching.discard();
      return None();
    } else if (catching.isFailed()) {
      return Error(catching.failure());
    }

    CHECK(catching.isReady()) << "Not expecting a discarded future!";

    elected = true;
    return index++;
  }
}


Result<uint64_t> Coordinator::demote()
{
  elected = false;
  return index - 1;
}


Result<uint64_t> Coordinator::append(
    const string& bytes,
    const Timeout& timeout)
{
  if (!elected) {
    return Error("Coordinator not elected");
  }

  Action action;
  action.set_position(index);
  action.set_promised(proposal);
  action.set_performed(proposal);
  action.set_type(Action::APPEND);
  Action::Append* append = action.mutable_append();
  append->set_bytes(bytes);

  Result<uint64_t> result = write(action, timeout);

  if (result.isSome()) {
    CHECK_EQ(result.get(), index);
    index++;
  }

  return result;
}


Result<uint64_t> Coordinator::truncate(
    uint64_t to,
    const Timeout& timeout)
{
  if (!elected) {
    return Error("Coordinator not elected");
  }

  Action action;
  action.set_position(index);
  action.set_promised(proposal);
  action.set_performed(proposal);
  action.set_type(Action::TRUNCATE);
  Action::Truncate* truncate = action.mutable_truncate();
  truncate->set_to(to);

  Result<uint64_t> result = write(action, timeout);

  if (result.isSome()) {
    CHECK_EQ(result.get(), index);
    index++;
  }

  return result;
}


Result<uint64_t> Coordinator::write(
    const Action& action,
    const Timeout& timeout)
{
  LOG(INFO) << "Coordinator attempting to write "
            << Action::Type_Name(action.type())
            << " action at position " << action.position()
            << " within " << timeout.remaining();

  CHECK(elected);

  CHECK(action.has_performed());
  CHECK(action.has_type());

  Future<WriteResponse> writing =
    log::write(quorum, network, proposal, action);

  if (!writing.await(timeout.remaining())) {
    writing.discard();
    return None();
  } else if (writing.isFailed()) {
    return Error(writing.failure());
  }

  CHECK(writing.isReady()) << "Not expecting a discarded future!";

  const WriteResponse& response = writing.get();
  if (!response.okay()) {
    elected = false;
    proposal = response.proposal();
    return Error("Coordinator demoted");
  } else {
    // TODO(jieyu): Currently, each log operation (append or truncate)
    // will write the same log content to the local disk twice: one
    // from log::write() and one from log::learn(). In the future, we
    // may want to use checksum to eliminate the duplicate disk write.
    Future<Nothing> learning = log::learn(network, action);

    // We need to make sure that learned message has been broadcasted,
    // thus has been enqueued.  Otherwise, our "missing" check below
    // will fail sometimes due to race condition.
    if (!learning.await(timeout.remaining())) {
      learning.discard();
      return None();
    } else if (learning.isFailed()) {
      return Error(learning.failure());
    }

    CHECK(learning.isReady()) << "Not expecting a discarded future!";

    // Make sure that the local replica has learned the newly written
    // log entry. Since messages are delivered and dispatched in order
    // locally, we should always have the new entry learned by now.
    Future<bool> checking = replica->missing(action.position());

    if (!checking.await(timeout.remaining())) {
      checking.discard();
      return None();
    } else if (checking.isFailed()) {
      return Error(checking.failure());
    }

    CHECK(checking.isReady()) << "Not expecting a discarded future!";

    CHECK(!checking.get());

    return action.position();
  }
}

} // namespace log {
} // namespace internal {
} // namespace mesos {
