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

#include <set>
#include <string>

#include <mesos/state/in_memory.hpp>
#include <mesos/state/storage.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/uuid.hpp>

using namespace process;

// Note that we don't add 'using std::set' here because we need
// 'std::' to disambiguate the 'set' member.
using std::string;

using mesos::internal::state::Entry;

namespace mesos {
namespace state {


class InMemoryStorageProcess : public Process<InMemoryStorageProcess>
{
public:
  InMemoryStorageProcess()
    : ProcessBase(process::ID::generate("in-memory-storage")) {}

  Option<Entry> get(const string& name)
  {
    return entries.get(name);
  }

  bool set(const Entry& entry, const id::UUID& uuid)
  {
    const Option<Entry>& option = entries.get(entry.name());

    if (option.isSome() && id::UUID::fromBytes(option->uuid()).get() != uuid) {
      return false;
    }

    entries.put(entry.name(), entry);
    return true;
  }

  bool expunge(const Entry& entry)
  {
    const Option<Entry>& option = entries.get(entry.name());

    if (option.isNone()) {
      return false;
    }

    if (id::UUID::fromBytes(option->uuid()).get() !=
        id::UUID::fromBytes(entry.uuid()).get()) {
      return false;
    }

    entries.erase(entry.name());
    return true;
  }

  std::set<string> names() // Use std:: to disambiguate 'set' member.
  {
    const hashset<string>& keys = entries.keys();
    return std::set<string>(keys.begin(), keys.end());
  }

private:
  hashmap<string, Entry> entries;
};



InMemoryStorage::InMemoryStorage()
{
  process = new InMemoryStorageProcess();
  spawn(process);
}


InMemoryStorage::~InMemoryStorage()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Option<Entry>> InMemoryStorage::get(const string& name)
{
  return dispatch(process, &InMemoryStorageProcess::get, name);
}


Future<bool> InMemoryStorage::set(const Entry& entry, const id::UUID& uuid)
{
  return dispatch(process, &InMemoryStorageProcess::set, entry, uuid);
}


Future<bool> InMemoryStorage::expunge(const Entry& entry)
{
  return dispatch(process, &InMemoryStorageProcess::expunge, entry);
}


Future<std::set<string>> InMemoryStorage::names()
{
  return dispatch(process, &InMemoryStorageProcess::names);
}

} // namespace state {
} // namespace mesos {
