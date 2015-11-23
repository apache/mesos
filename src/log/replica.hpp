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

#ifndef __LOG_REPLICA_HPP__
#define __LOG_REPLICA_HPP__

#include <stdint.h>

#include <list>
#include <string>

#include <process/future.hpp>
#include <process/pid.hpp>
#include <process/protobuf.hpp>

#include <stout/interval.hpp>

#include "messages/log.hpp"

namespace mesos {
namespace internal {
namespace log {

namespace protocol {

// Some replica protocol declarations.
extern Protocol<PromiseRequest, PromiseResponse> promise;
extern Protocol<WriteRequest, WriteResponse> write;
extern Protocol<RecoverRequest, RecoverResponse> recover;

} // namespace protocol {


// Forward declaration.
class ReplicaProcess;


class Replica
{
public:
  // Constructs a new replica process using specified path to a
  // directory for storing the underlying log. If a replica starts
  // with an empty log, it will not be allowed to vote (i.e., cannot
  // reply to any request except the recover request). The recover
  // process will later decide if this replica can be re-allowed to
  // vote depending on the status of other replicas.
  explicit Replica(const std::string& path);
  virtual ~Replica();

  // Returns all the actions between the specified positions, unless
  // those positions are invalid, in which case returns an error.
  process::Future<std::list<Action>> read(
      uint64_t from,
      uint64_t to) const;

  // Returns true if the specified position is missing in the log
  // (i.e., unlearned or holes).
  process::Future<bool> missing(uint64_t position) const;

  // Returns missing positions in the log (i.e., unlearned or holes)
  // within the specified range [from, to]. We use interval set, a
  // more compact representation of set, to store missing positions.
  process::Future<IntervalSet<uint64_t>> missing(
      uint64_t from,
      uint64_t to) const;

  // Returns the beginning position of the log.
  process::Future<uint64_t> beginning() const;

  // Returns the last written position in the log.
  process::Future<uint64_t> ending() const;

  // Returns the current status of this replica.
  process::Future<Metadata::Status> status() const;

  // Returns the highest implicit promise this replica has given.
  process::Future<uint64_t> promised() const;

  // Updates the status of this replica. Returns true if status was
  // updated successfully, false otherwise. Made "virtual" for
  // mocking in tests.
  virtual process::Future<bool> update(const Metadata::Status& status);

  // Returns the PID associated with this replica.
  process::PID<ReplicaProcess> pid() const;

private:
  ReplicaProcess* process;
};

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_REPLICA_HPP__
