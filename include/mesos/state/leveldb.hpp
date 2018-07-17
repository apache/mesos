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
// limitations under the License

#ifndef __MESOS_STATE_LEVELDB_HPP__
#define __MESOS_STATE_LEVELDB_HPP__

#include <set>
#include <string>

#include <mesos/state/storage.hpp>

#include <process/future.hpp>

#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

namespace mesos {
namespace state {

// More forward declarations.
class LevelDBStorageProcess;


class LevelDBStorage : public mesos::state::Storage
{
public:
  explicit LevelDBStorage(const std::string& path);
  ~LevelDBStorage() override;

  // Storage implementation.
  process::Future<Option<internal::state::Entry>> get(
      const std::string& name) override;
  process::Future<bool> set(
      const internal::state::Entry& entry,
      const id::UUID& uuid) override;
  process::Future<bool> expunge(const internal::state::Entry& entry) override;
  process::Future<std::set<std::string>> names() override;

private:
  LevelDBStorageProcess* process;
};

} // namespace state {
} // namespace mesos {

#endif // __MESOS_STATE_LEVELDB_HPP__
