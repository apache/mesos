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

#ifndef __MESOS_STATE_STATE_HPP__
#define __MESOS_STATE_STATE_HPP__

#include <set>
#include <string>

#include <mesos/state/storage.hpp>

#include <process/deferred.hpp> // TODO(benh): This is required by Clang.
#include <process/future.hpp>

#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/some.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

namespace mesos {
namespace state {

// An abstraction of "state" (possibly between multiple distributed
// components) represented by "variables" (effectively key/value
// pairs). Variables are versioned such that setting a variable in the
// state will only succeed if the variable has not changed since last
// fetched. Varying implementations of state provide varying
// replicated guarantees.
//
// Note that the semantics of 'fetch' and 'store' provide
// atomicity. That is, you cannot store a variable that has changed
// since you did the last fetch. That is, if a store succeeds then no
// other writes have been performed on the variable since your fetch.
//
// Example:
//
//   Storage* storage = new ZooKeeperStorage();
//   State* state = new State(storage);
//   Future<Variable> variable = state->fetch("slaves");
//   std::string value = update(variable.value());
//   variable = variable.mutate(value);
//   state->store(variable);

// Forward declarations.
class State;


// Wrapper around a state "entry" to force immutability.
class Variable
{
public:
  std::string value() const
  {
    return entry.value();
  }

  Variable mutate(const std::string& value) const
  {
    Variable variable(*this);
    variable.entry.set_value(value);
    return variable;
  }

private:
  friend class State; // Creates and manages variables.

  explicit Variable(const internal::state::Entry& _entry)
    : entry(_entry)
  {}

  internal::state::Entry entry; // Not const to keep Variable assignable.
};


class State
{
public:
  explicit State(Storage* _storage) : storage(_storage) {}
  virtual ~State() {}

  // Returns a variable from the state, creating a new one if one
  // previously did not exist (or an error if one occurs).
  process::Future<Variable> fetch(const std::string& name);

  // Returns the variable specified if it was successfully stored in
  // the state, otherwise returns none if the version of the variable
  // was no longer valid, or an error if one occurs.
  process::Future<Option<Variable>> store(const Variable& variable);

  // Returns true if successfully expunged the variable from the state.
  process::Future<bool> expunge(const Variable& variable);

  // Returns the collection of variable names in the state.
  process::Future<std::set<std::string>> names();

private:
  // Helpers to handle future results from fetch and swap. We make
  // these static members of State for friend access to Variable's
  // constructor.
  static process::Future<Variable> _fetch(
      const std::string& name,
      const Option<internal::state::Entry>& option);

  static process::Future<Option<Variable>> _store(
      const internal::state::Entry& entry,
      const bool& b); // TODO(benh): Remove 'const &' after fixing libprocess.

  Storage* storage;
};


inline process::Future<Variable> State::fetch(const std::string& name)
{
  return storage->get(name)
    .then(lambda::bind(&State::_fetch, name, lambda::_1));
}


inline process::Future<Variable> State::_fetch(
    const std::string& name,
    const Option<internal::state::Entry>& option)
{
  if (option.isSome()) {
    return Variable(option.get());
  }

  // Otherwise, construct a Variable with a new Entry (with a random
  // UUID and no value to start).
  internal::state::Entry entry;
  entry.set_name(name);
  entry.set_uuid(id::UUID::random().toBytes());

  return Variable(entry);
}


inline process::Future<Option<Variable>> State::store(const Variable& variable)
{
  // Note that we try and swap an entry even if the value didn't change!
  id::UUID uuid = id::UUID::fromBytes(variable.entry.uuid()).get();

  // Create a new entry to replace the existing entry provided the
  // UUID matches.
  internal::state::Entry entry;
  entry.set_name(variable.entry.name());
  entry.set_uuid(id::UUID::random().toBytes());
  entry.set_value(variable.entry.value());

  return storage->set(entry, uuid)
    .then(lambda::bind(&State::_store, entry, lambda::_1));
}


inline process::Future<Option<Variable>> State::_store(
    const internal::state::Entry& entry,
    const bool& b) // TODO(benh): Remove 'const &' after fixing libprocess.
{
  if (b) {
    return Some(Variable(entry));
  }

  return None();
}


inline process::Future<bool> State::expunge(const Variable& variable)
{
  return storage->expunge(variable.entry);
}


inline process::Future<std::set<std::string>> State::names()
{
  return storage->names();
}

} // namespace state {
} // namespace mesos {

#endif // __MESOS_STATE_STATE_HPP__
