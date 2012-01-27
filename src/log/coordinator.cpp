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

#include <process/dispatch.hpp>
#include <process/future.hpp>

#include "common/foreach.hpp"

#include "log/coordinator.hpp"
#include "log/replica.hpp"

using std::list;
using std::pair;
using std::set;
using std::string;


namespace mesos {
namespace internal {
namespace log {

Coordinator::Coordinator(int _quorum,
                         Replica* _replica,
                         Network* _network)
  : elected(false),
    quorum(_quorum),
    replica(_replica),
    network(_network),
    id(0),
    index(0) {}


Coordinator::~Coordinator() {}


Result<uint64_t> Coordinator::elect(const Timeout& timeout)
{
  LOG(INFO) << "Coordinator attempting to get elected within "
            << timeout.remaining() << " seconds";

  if (elected) {
    // TODO(benh): No-op instead of error?
    return Result<uint64_t>::error("Coordinator already elected");
  }

  // Get the highest known promise from our local replica.
  Future<uint64_t> promise = replica->promised();

  if (!promise.await(timeout.remaining())) {
    return Result<uint64_t>::none();
  } else if (promise.isFailed()) {
    return Result<uint64_t>::error(promise.failure());
  }

  CHECK(promise.isReady()) << "Not expecting a discarded future!";

  id = std::max(id, promise.get()) + 1; // Try the next highest!

  PromiseRequest request;
  request.set_id(id);

  // Broadcast the request to the network.
  set<Future<PromiseResponse> > futures =
    broadcast(protocol::promise, request);

  int okays = 0;

  do {
    Future<Future<PromiseResponse> > future = select(futures);
    if (future.await(timeout.remaining())) {
      CHECK(future.get().isReady());
      const PromiseResponse& response = future.get().get();
      if (!response.okay()) {
        return Result<uint64_t>::none(); // Lost an election, but can retry.
      } else if (response.okay()) {
        CHECK(response.has_position());
        index = std::max(index, response.position());
        okays++;
        if (okays >= quorum) {
          break;
        }
      }
      futures.erase(future.get());
    }
  } while (timeout.remaining() > 0);

  // Discard the remaining futures.
  discard(futures);

  // Either we have a quorum or we timed out.
  if (okays >= quorum) {
    LOG(INFO) << "Coordinator elected, attempting to fill missing positions";
    elected = true;

    // Need to "catchup" local replica (i.e., fill in any unlearned
    // and/or missing positions) so that we can do local reads.
    // Usually we could do this lazily, however, a local learned
    // position might have been truncated, so we actually need to
    // catchup the local replica all the way to the end of the log
    // before we can perform any up-to-date local reads.

    Future<set<uint64_t> > positions = replica->missing(index);

    if (!positions.await(timeout.remaining())) {
      elected = false;
      return Result<uint64_t>::none();
    } else if (positions.isFailed()) {
      elected = false;
      return Result<uint64_t>::error(positions.failure());
    }

    CHECK(positions.isReady()) << "Not expecting a discarded future!";

    foreach (uint64_t position, positions.get()) {
      Result<Action> result = fill(position, timeout);
      if (result.isError()) {
        elected = false;
        return Result<uint64_t>::error(result.error());
      } else if (result.isNone()) {
        elected = false;
        return Result<uint64_t>::none();
      } else {
        CHECK(result.isSome());
        CHECK(result.get().position() == position);
      }
    }

    index += 1;
    return index - 1;
  }

  // Timed out ...
  LOG(INFO) << "Coordinator timed out while trying to get elected";
  return Result<uint64_t>::none();
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
    return Result<uint64_t>::error("Coordinator not elected");
  }

  Action action;
  action.set_position(index);
  action.set_promised(id);
  action.set_performed(id);
  action.set_type(Action::APPEND);
  Action::Append* append = action.mutable_append();
  append->set_bytes(bytes);

  Result<uint64_t> result = write(action, timeout);

  if (result.isSome()) {
    CHECK(result.get() == index);
    index++;
  }

  return result;
}


Result<uint64_t> Coordinator::truncate(
    uint64_t to,
    const Timeout& timeout)
{
  if (!elected) {
    return Result<uint64_t>::error("Coordinator not elected");
  }

  Action action;
  action.set_position(index);
  action.set_promised(id);
  action.set_performed(id);
  action.set_type(Action::TRUNCATE);
  Action::Truncate* truncate = action.mutable_truncate();
  truncate->set_to(to);

  Result<uint64_t> result = write(action, timeout);

  if (result.isSome()) {
    CHECK(result.get() == index);
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
            << " within " << timeout.remaining() << " seconds";

  CHECK(elected);

  CHECK(action.has_performed());
  CHECK(action.has_type());

  // TODO(benh): Eliminate this special case hack?
  if (quorum == 1) {
    Result<uint64_t> result = commit(action);
    if (result.isError()) {
      return Result<uint64_t>::error(result.error());
    } else if (result.isNone()) {
      return Result<uint64_t>::none();
    } else {
      CHECK(result.isSome());
      return action.position();
    }
  }

  WriteRequest request;
  request.set_id(id);
  request.set_position(action.position());
  request.set_type(action.type());
  switch (action.type()) {
    case Action::NOP:
      CHECK(action.has_nop());
      request.mutable_nop();
      break;
    case Action::APPEND:
      CHECK(action.has_append());
      request.mutable_append()->MergeFrom(action.append());
      break;
    case Action::TRUNCATE:
      CHECK(action.has_truncate());
      request.mutable_truncate()->MergeFrom(action.truncate());
      break;
    default:
      LOG(FATAL) << "Unknown Action::Type!";
  }

  // Broadcast the request to the network *excluding* the local replica.
  set<Future<WriteResponse> > futures =
    remotecast(protocol::write, request);

  int okays = 0;

  do {
    Future<Future<WriteResponse> > future = select(futures);
    if (future.await(timeout.remaining())) {
      CHECK(future.get().isReady());
      const WriteResponse& response = future.get().get();
      CHECK(response.id() == request.id());
      CHECK(response.position() == request.position());
      if (!response.okay()) {
        elected = false;
        return Result<uint64_t>::error("Coordinator demoted");
      } else if (response.okay()) {
        if (++okays >= (quorum - 1)) { // N.B. Using (quorum - 1) here!
          // Got enough remote okays, discard the remaining futures
          // and try and commit the action locally.
          discard(futures);
          Result<uint64_t> result = commit(action);
          if (result.isError()) {
            return Result<uint64_t>::error(result.error());
          } else if (result.isNone()) {
            return Result<uint64_t>::none();
          } else {
            CHECK(result.isSome());
            return action.position();
          }
        }
      }
      futures.erase(future.get());
    }
  } while (timeout.remaining() > 0);

  // Timed out ... discard remaining futures.
  discard(futures);
  return Result<uint64_t>::none();
}


Result<uint64_t> Coordinator::commit(const Action& action)
{
  LOG(INFO) << "Coordinator attempting to commit "
            << Action::Type_Name(action.type())
            << " action at position " << action.position();

  CHECK(elected);

  WriteRequest request;
  request.set_id(id);
  request.set_position(action.position());
  request.set_learned(true); // A commit is just a learned write.
  request.set_type(action.type());
  switch (action.type()) {
    case Action::NOP:
      CHECK(action.has_nop());
      request.mutable_nop();
      break;
    case Action::APPEND:
      CHECK(action.has_append());
      request.mutable_append()->MergeFrom(action.append());
      break;
    case Action::TRUNCATE:
      CHECK(action.has_truncate());
      request.mutable_truncate()->MergeFrom(action.truncate());
      break;
    default:
      LOG(FATAL) << "Unknown Action::Type!";
  }

  //  TODO(benh): Add a non-message based way to do this write.
  Future<WriteResponse> future = protocol::write(replica->pid(), request);

  // We send a write request to the *local* replica just as the
  // others: asynchronously via messages. However, rather than add the
  // complications of dealing with timeouts for local operations
  // (especially since we are trying to commit something), we make
  // things simpler and block on the response from the local replica.
  // Maybe we can let it timeout, but consider it a failure? This
  // might be sound because we don't send the learned messages ... so
  // this should be the same as if we just failed before we even do
  // the write ... a client should just retry this write later.

  future.await(); // TODO(benh): Don't wait forever, see comment above.

  if (future.isFailed()) {
    return Result<uint64_t>::error(future.failure());
  }

  CHECK(future.isReady()) << "Not expecting a discarded future!";

  const WriteResponse& response = future.get();
  CHECK(response.id() == request.id());
  CHECK(response.position() == request.position());

  if (!response.okay()) {
    elected = false;
    return Result<uint64_t>::error("Coordinator demoted");
  }

  // Commit successful, send a learned message to the network
  // *excluding* the local replica and return the position.

  LearnedMessage message;
  message.mutable_action()->MergeFrom(action);

  if (!action.has_learned() || !action.learned()) {
    message.mutable_action()->set_learned(true);
  }

  remotecast(message);

  return action.position();
}


Result<Action> Coordinator::fill(uint64_t position, const Timeout& timeout)
{
  LOG(INFO) << "Coordinator attempting to fill position "
            << position << " in the log";

  CHECK(elected);

  PromiseRequest request;
  request.set_id(id);
  request.set_position(position);

  // Broadcast the request to the network.
  set<Future<PromiseResponse> > futures =
    broadcast(protocol::promise, request);

  list<PromiseResponse> responses;

  do {
    Future<Future<PromiseResponse> > future = select(futures);
    if (future.await(timeout.remaining())) {
      CHECK(future.get().isReady());
      const PromiseResponse& response = future.get().get();
      CHECK(response.id() == request.id());
      if (!response.okay()) {
        elected = false;
        return Result<Action>::error("Coordinator demoted");
      } else if (response.okay()) {
        responses.push_back(response);
        if (responses.size() >= quorum) {
          break;
        }
      }
      futures.erase(future.get());
    }
  } while (timeout.remaining() > 0);

  // Discard the remaining futures.
  discard(futures);

  // Either have a quorum or we timed out.
  if (responses.size() >= quorum) {
    // Check the responses for a learned action, otherwise, pick the
    // action with the higest performed id or a no-op if no responses
    // include performed actions.
    Action action;
    foreach (const PromiseResponse& response, responses) {
      if (response.has_action()) {
        CHECK(response.action().position() == position);
        if (response.action().has_learned() && response.action().learned()) {
          // Received a learned action, try and commit locally.
          Result<uint64_t> result = commit(response.action());
          if (result.isError()) {
            return Result<Action>::error(result.error());
          } else if (result.isNone()) {
            return Result<Action>::none();
          } else {
            CHECK(result.isSome());
            return response.action();
          }
        } else if (response.action().has_performed() &&
                   (!action.has_performed() ||
                    response.action().performed() > action.performed())) {
          action = response.action();
        }
      } else {
        CHECK(response.has_position());
        CHECK(response.position() == position);
      }
    }

    // Use a no-op if no known action has been performed.
    if (!action.has_performed()) {
      action.set_position(position);
      action.set_promised(id);
      action.set_performed(id);
      action.set_type(Action::NOP);
      action.mutable_nop();
    } else {
      action.set_performed(id);
    }

    Result<uint64_t> result = write(action, timeout);

    if (result.isError()) {
      return Result<Action>::error(result.error());
    } else if (result.isNone()) {
      return Result<Action>::none();
    } else {
      CHECK(result.isSome());
      return action;
    }
  }

  // Timed out ...
  return Result<Action>::none();
}


template <typename Req, typename Res>
set<Future<Res> > Coordinator::broadcast(
    const Protocol<Req, Res>& protocol,
    const Req& req)
{
  Future<set<Future<Res> > > futures =
    network->broadcast(protocol, req);
  futures.await();
  CHECK(futures.isReady());
  return futures.get();
}


template <typename Req, typename Res>
set<Future<Res> > Coordinator::remotecast(
    const Protocol<Req, Res>& protocol,
    const Req& req)
{
  set<UPID> filter;
  filter.insert(replica->pid());
  Future<set<Future<Res> > > futures =
    network->broadcast(protocol, req, filter);
  futures.await();
  CHECK(futures.isReady());
  return futures.get();
}


template <typename M>
void Coordinator::remotecast(const M& m)
{
  set<UPID> filter;
  filter.insert(replica->pid());
  network->broadcast(m, filter);
}

} // namespace log {
} // namespace internal {
} // namespace mesos {
