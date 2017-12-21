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

#ifndef __MESOS_LOG_LOG_HPP__
#define __MESOS_LOG_LOG_HPP__

#include <stdint.h>

#include <list>
#include <set>
#include <string>

#include <mesos/zookeeper/authentication.hpp>

#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>

namespace mesos {
namespace internal {
namespace log {

// Forward declarations.
class LogProcess;
class LogReaderProcess;
class LogWriterProcess;

} // namespace log {
} // namespace internal {
} // namespace mesos {


namespace mesos {
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
    bool operator==(const Position& that) const
    {
      return value == that.value;
    }

    bool operator<(const Position& that) const
    {
      return value < that.value;
    }

    bool operator<=(const Position& that) const
    {
      return value <= that.value;
    }

    bool operator>(const Position& that) const
    {
      return value > that.value;
    }

    bool operator>=(const Position& that) const
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
    friend class Writer;
    friend class internal::log::LogReaderProcess;
    friend class internal::log::LogWriterProcess;

    /*implicit*/ Position(uint64_t _value) : value(_value) {}

    uint64_t value;
  };

  class Entry
  {
  public:
    Position position;
    std::string data;

  private:
    friend class internal::log::LogReaderProcess;

    Entry(const Position& _position, const std::string& _data)
      : position(_position), data(_data) {}
  };

  class Reader
  {
  public:
    explicit Reader(Log* log);
    ~Reader();

    // Returns all entries between the specified positions, unless
    // those positions are invalid, in which case returns an error.
    process::Future<std::list<Entry>> read(
        const Position& from,
        const Position& to);

    // Returns the beginning position of the log from the perspective
    // of the local replica (which may be out of date if the log has
    // been opened and truncated while this replica was partitioned).
    process::Future<Position> beginning();

    // Returns the ending (i.e., last) position of the log from the
    // perspective of the local replica (which may be out of date if
    // the log has been opened and appended to while this replica was
    // partitioned).
    process::Future<Position> ending();

    // Launches the catch-up process. Returns the ending position of
    // the caught-up range.
    process::Future<Position> catchup();

  private:
    internal::log::LogReaderProcess* process;
  };

  class Writer
  {
  public:
    // Creates a new writer associated with the specified log. Only
    // one writer (local or remote) can be valid at any point in
    // time. A writer becomes invalid if either Writer::append or
    // Writer::truncate return None, in which case, the writer (or
    // another writer) must be restarted.
    explicit Writer(Log* log);
    ~Writer();

    // Attempts to get a promise (from the log's replicas) for
    // exclusive writes, i.e., no other writer's will be able to
    // perform append and truncate operations. Returns the ending
    // position of the log or none if the promise to exclusively write
    // could not be attained but may be retried.
    process::Future<Option<Position>> start();

    // Attempts to append the specified data to the log. Returns the
    // new ending position of the log or 'none' if this writer has
    // lost its promise to exclusively write (which can be reacquired
    // by invoking Writer::start).
    process::Future<Option<Position>> append(const std::string& data);

    // Attempts to truncate the log up to but not including the
    // specificed position. Returns the new ending position of the log
    // or 'none' if this writer has lost its promise to exclusively
    // write (which can be reacquired by invoking Writer::start).
    process::Future<Option<Position>> truncate(const Position& to);

  private:
    internal::log::LogWriterProcess* process;
  };

  // Creates a new replicated log that assumes the specified quorum
  // size, is backed by a file at the specified path, and coordinates
  // with other replicas via the set of process PIDs.
  Log(int quorum,
      const std::string& path,
      const std::set<process::UPID>& pids,
      bool autoInitialize = false,
      const Option<std::string>& metricsPrefix = None());

  // Creates a new replicated log that assumes the specified quorum
  // size, is backed by a file at the specified path, and coordinates
  // with other replicas associated with the specified ZooKeeper
  // servers, timeout, and znode.
  Log(int quorum,
      const std::string& path,
      const std::string& servers,
      const Duration& timeout,
      const std::string& znode,
      const Option<zookeeper::Authentication>& auth = None(),
      bool autoInitialize = false,
      const Option<std::string>& metricsPrefix = None());

  ~Log();

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
  friend class internal::log::LogReaderProcess;
  friend class internal::log::LogWriterProcess;

  internal::log::LogProcess* process;
};

} // namespace log {
} // namespace mesos {

#endif // __MESOS_LOG_LOG_HPP__
