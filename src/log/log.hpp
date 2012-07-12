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

#ifndef __LOG_HPP__
#define __LOG_HPP__

#include <list>
#include <set>
#include <string>

#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/foreach.hpp>
#include <stout/result.hpp>
#include <stout/time.hpp>
#include <stout/try.hpp>

#include "log/coordinator.hpp"
#include "log/replica.hpp"

#include "zookeeper/group.hpp"

namespace mesos {
namespace internal {
namespace log {

class Log
{
public:
  // Forward declarations.
  class Reader;
  class Writer;

  class Position
  {
  public:
    bool operator == (const Position& that) const
    {
      return value == that.value;
    }

    bool operator < (const Position& that) const
    {
      return value < that.value;
    }

    bool operator <= (const Position& that) const
    {
      return value <= that.value;
    }

    bool operator > (const Position& that) const
    {
      return value > that.value;
    }

    bool operator >= (const Position& that) const
    {
      return value >= that.value;
    }

    // Returns an "identity" off this position, useful for serializing
    // to logs or across communication mediums.
    std::string identity() const
    {
      CHECK(sizeof(value) == 8);
      char bytes[8];
      bytes[0] =(0xff & (value >> 56));
      bytes[1] = (0xff & (value >> 48));
      bytes[2] = (0xff & (value >> 40));
      bytes[3] = (0xff & (value >> 32));
      bytes[4] = (0xff & (value >> 24));
      bytes[5] = (0xff & (value >> 16));
      bytes[6] = (0xff & (value >> 8));
      bytes[7] = (0xff & value);
      return std::string(bytes, sizeof(bytes));
    }

  private:
    friend class Log;
    friend class Reader;
    friend class Writer;
    Position(uint64_t _value) : value(_value) {}
    uint64_t value;
  };

  class Entry
  {
  public:
    Position position;
    std::string data;

  private:
    friend class Reader;
    friend class Writer;
    Entry(const Position& _position, const std::string& _data)
      : position(_position), data(_data) {}
  };

  class Reader
  {
  public:
    Reader(Log* log);
    ~Reader();

    // Returns all entries between the specified positions, unless
    // those positions are invalid, in which case returns an error.
    Result<std::list<Entry> > read(const Position& from,
                                   const Position& to,
                                   const seconds& timeout);

    // Returns the beginning position of the log from the perspective
    // of the local replica (which may be out of date if the log has
    // been opened and truncated while this replica was partitioned).
    Position beginning();

    // Returns the ending (i.e., last) position of the log from the
    // perspective of the local replica (which may be out of date if
    // the log has been opened and appended to while this replica was
    // partitioned).
    Position ending();

  private:
    Replica* replica;
  };

  class Writer
  {
  public:
    // Creates a new writer associated with the specified log. Only
    // one writer (local and remote) is valid at a time. A writer
    // becomes invalid if any operation returns an error, and a new
    // writer must be created in order perform subsequent operations.
    Writer(Log* log, const seconds& timeout, int retries = 3);
    ~Writer();

    // Attempts to append the specified data to the log. A none result
    // means the operation timed out, otherwise the new ending
    // position of the log is returned or an error. Upon error a new
    // Writer must be created.
    Result<Position> append(const std::string& data, const seconds& timeout);

    // Attempts to truncate the log up to but not including the
    // specificed position. A none result means the operation timed
    // out, otherwise the new ending position of the log is returned
    // or an error. Upon error a new Writer must be created.
    Result<Position> truncate(const Position& to, const seconds& timeout);

  private:
    Option<std::string> error;
    Coordinator coordinator;
  };

  // Creates a new replicated log that assumes the specified quorum
  // size, is backed by a file at the specified path, and coordiantes
  // with other replicas via the set of process PIDs.
  Log(int _quorum,
      const std::string& path,
      const std::set<process::UPID>& pids)
    : group(NULL)
  {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    quorum = _quorum;

    replica = new Replica(path);

    network = new Network(pids);

    // Don't forget to add our own replica!
    network->add(replica->pid());
  }

  // Creates a new replicated log that assumes the specified quorum
  // size, is backed by a file at the specified path, and coordiantes
  // with other replicas associated with the specified ZooKeeper
  // servers, timeout, and znode.
  Log(int _quorum,
      const std::string& path,
      const std::string& servers,
      const seconds& timeout,
      const std::string& znode,
      const Option<zookeeper::Authentication>& auth
        = Option<zookeeper::Authentication>::none())
  {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    quorum = _quorum;

    LOG(INFO) << "Creating a new log replica";

    replica = new Replica(path);

    group = new zookeeper::Group(servers, timeout, znode, auth);
    network = new ZooKeeperNetwork(group);

    // Need to add our replica to the ZooKeeper group!
    LOG(INFO) << "Attempting to join replica to ZooKeeper group";

    membership = group->join(replica->pid())
      .onFailed(executor.defer(lambda::bind(&Log::failed, this, lambda::_1)))
      .onDiscarded(executor.defer(lambda::bind(&Log::discarded, this)));

    group->watch()
      .onReady(executor.defer(lambda::bind(&Log::watch, this, lambda::_1)))
      .onFailed(executor.defer(lambda::bind(&Log::failed, this, lambda::_1)))
      .onDiscarded(executor.defer(lambda::bind(&Log::discarded, this)));
  }

  ~Log()
  {
    delete network;
    delete group;
    delete replica;
  }

  // Returns a position based off of the bytes recovered from
  // Position.identity().
  Position position(const std::string& identity) const
  {
    CHECK(identity.size() == 8);
    const char* bytes = identity.c_str();
    uint64_t value =
      ((uint64_t) (bytes[0] & 0xff) << 56) |
      ((uint64_t) (bytes[1] & 0xff) << 48) |
      ((uint64_t) (bytes[2] & 0xff) << 40) |
      ((uint64_t) (bytes[3] & 0xff) << 32) |
      ((uint64_t) (bytes[4] & 0xff) << 24) |
      ((uint64_t) (bytes[5] & 0xff) << 16) |
      ((uint64_t) (bytes[6] & 0xff) << 8) |
      ((uint64_t) (bytes[7] & 0xff));
    return Position(value);
  }
private:
  friend class Reader;
  friend class Writer;

  // TODO(benh): Factor this out into some sort of "membership renewer".
  void watch(const std::set<zookeeper::Group::Membership>& memberships);
  void failed(const std::string& message) const;
  void discarded() const;

  zookeeper::Group* group;
  process::Future<zookeeper::Group::Membership> membership;
  process::Executor executor;

  int quorum;

  Replica* replica;
  Network* network;
};


Log::Reader::Reader(Log* log)
  : replica(log->replica) {}


Log::Reader::~Reader() {}


Result<std::list<Log::Entry> > Log::Reader::read(
    const Log::Position& from,
    const Log::Position& to,
    const seconds& timeout)
{
  process::Future<std::list<Action> > actions =
    replica->read(from.value, to.value);

  if (!actions.await(timeout.value)) {
    return Result<std::list<Log::Entry> >::none();
  } else if (actions.isFailed()) {
    return Result<std::list<Log::Entry> >::error(actions.failure());
  }

  CHECK(actions.isReady()) << "Not expecting discarded future!";

  std::list<Log::Entry> entries;

  uint64_t position = from.value;

  foreach (const Action& action, actions.get()) {
    // Ensure read range is valid.
    if (!action.has_performed() ||
        !action.has_learned() ||
        !action.learned()) {
      return Result<std::list<Log::Entry> >::error(
          "Bad read range (includes pending entries)");
    } else if (position++ != action.position()) {
      return Result<std::list<Log::Entry> >::error(
          "Bad read range (includes missing entries)");
    }

    // And only return appends.
    CHECK(action.has_type());
    if (action.type() == Action::APPEND) {
      entries.push_back(Entry(action.position(), action.append().bytes()));
    }
  }

  return entries;
}


Log::Position Log::Reader::beginning()
{
  // TODO(benh): Take a timeout and return an Option.
  process::Future<uint64_t> value = replica->beginning();
  value.await();
  CHECK(value.isReady()) << "Not expecting a failed or discarded future!";
  return Log::Position(value.get());
}


Log::Position Log::Reader::ending()
{
  // TODO(benh): Take a timeout and return an Option.
  process::Future<uint64_t> value = replica->ending();
  value.await();
  CHECK(value.isReady()) << "Not expecting a failed or discarded future!";
  return Log::Position(value.get());
}


Log::Writer::Writer(Log* log, const seconds& timeout, int retries)
  : coordinator(log->quorum, log->replica, log->network),
    error(Option<std::string>::none())
{
  do {
    Result<uint64_t> result = coordinator.elect(Timeout(timeout.value));
    if (result.isNone()) {
      retries--;
    } else if (result.isSome()) {
      break;
    } else {
      error = result.error();
      break;
    }
  } while (retries > 0);
}


Log::Writer::~Writer()
{
  coordinator.demote();
}


Result<Log::Position> Log::Writer::append(
    const std::string& data,
    const seconds& timeout)
{
  if (error.isSome()) {
    return Result<Log::Position>::error(error.get());
  }

  LOG(INFO) << "Attempting to append " << data.size() << " bytes to the log";

  Result<uint64_t> result = coordinator.append(data, Timeout(timeout.value));

  if (result.isError()) {
    error = result.error();
    return Result<Log::Position>::error(error.get());
  } else if (result.isNone()) {
    return Result<Log::Position>::none();
  }

  CHECK(result.isSome());

  return Log::Position(result.get());
}


Result<Log::Position> Log::Writer::truncate(
    const Log::Position& to,
    const seconds& timeout)
{
  if (error.isSome()) {
    return Result<Log::Position>::error(error.get());
  }

  LOG(INFO) << "Attempting to truncate the log to " << to.value;

  Result<uint64_t> result =
    coordinator.truncate(to.value, Timeout(timeout.value));

  if (result.isError()) {
    error = result.error();
    return Result<Log::Position>::error(error.get());
  } else if (result.isNone()) {
    return Result<Log::Position>::none();
  }

  CHECK(result.isSome());

  return Log::Position(result.get());
}


void Log::watch(const std::set<zookeeper::Group::Membership>& memberships)
{
  if (membership.isReady() && memberships.count(membership.get()) == 0) {
    // Our replica's membership must have expired, join back up.
    LOG(INFO) << "Renewing replica group membership";
    membership = group->join(replica->pid())
      .onFailed(executor.defer(lambda::bind(&Log::failed, this, lambda::_1)))
      .onDiscarded(executor.defer(lambda::bind(&Log::discarded, this)));
  }

  group->watch(memberships)
    .onReady(executor.defer(lambda::bind(&Log::watch, this, lambda::_1)))
    .onFailed(executor.defer(lambda::bind(&Log::failed, this, lambda::_1)))
    .onDiscarded(executor.defer(lambda::bind(&Log::discarded, this)));
}


void Log::failed(const std::string& message) const
{
  LOG(FATAL) << "Failed to participate in ZooKeeper group: " << message;
}


void Log::discarded() const
{
  LOG(FATAL) << "Not expecting future to get discarded!";
}

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_HPP__
