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

#ifndef __MESOS_STATE_STORAGE_HPP__
#define __MESOS_STATE_STORAGE_HPP__

#include <set>
#include <string>

#include <mesos/state/state.pb.h>

#include <process/future.hpp>

#include <stout/option.hpp>
#include <stout/uuid.hpp>

namespace mesos {
namespace state {

class Storage
{
public:
  Storage() {}
  virtual ~Storage() {}

  // Get and set state entries, factored out to allow Storage
  // implementations to be agnostic of Variable. Note that set acts
  // like a "test-and-set" by requiring the existing entry to have the
  // specified UUID.
  virtual process::Future<Option<internal::state::Entry>> get(
      const std::string& name) = 0;
  virtual process::Future<bool> set(
      const internal::state::Entry& entry,
      const id::UUID& uuid) = 0;

  // Returns true if successfully expunged the variable from the state.
  virtual process::Future<bool> expunge(
      const internal::state::Entry& entry) = 0;

  // Returns the collection of variable names in the state.
  virtual process::Future<std::set<std::string>> names() = 0;
};

} // namespace state {
} // namespace mesos {

#endif // __MESOS_STATE_STORAGE_HPP__
