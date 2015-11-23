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

#ifndef __LOG_STORAGE_HPP__
#define __LOG_STORAGE_HPP__

#include <stdint.h>

#include <string>

#include <stout/interval.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "messages/log.hpp"

namespace mesos {
namespace internal {
namespace log {

// Abstract interface for reading and writing records.
class Storage
{
public:
  struct State
  {
    Metadata metadata; // The metadata for the replica.
    uint64_t begin; // Beginning position of the log.
    uint64_t end; // Ending position of the log.

    // Note that we use interval set here to store learned/unlearned
    // positions in a more compact way. Adjacent positions will be
    // merged and represented using an interval.
    IntervalSet<uint64_t> learned;
    IntervalSet<uint64_t> unlearned;
  };

  virtual ~Storage() {}

  virtual Try<State> restore(const std::string& path) = 0;
  virtual Try<Nothing> persist(const Metadata& metadata) = 0;
  virtual Try<Nothing> persist(const Action& action) = 0;
  virtual Try<Action> read(uint64_t position) = 0;
};

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_STORAGE_HPP__
