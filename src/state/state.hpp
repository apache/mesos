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

#ifndef __STATE_STATE_HPP__
#define __STATE_STATE_HPP__

#include <string>
#include <vector>

#include <process/future.hpp>

#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "logging/logging.hpp"

#include "messages/state.hpp"

#include "state/serializer.hpp"

namespace mesos {
namespace internal {
namespace state {

// An abstraction of "state" (possibly between multiple distributed
// components) represented by "variables" (effectively key/value
// pairs). Variables are versioned such that setting a variable in the
// state will only succeed if the variable has not changed since last
// fetched. Varying implementations of state provide varying
// replicated guarantees.
//
// Note that the semantics of 'get' and 'set' provide atomicity. That
// is, you can not set a variable that has changed since you did the
// last get. That is, if a set succeeds then no other writes have been
// performed on the variable since your get.

// Example:

//   State<ProtobufSerializer>* state = new ZooKeeperState<ProtobufSerializer>();
//   Future<Variable<Slaves> > variable = state->get<Slaves>("slaves");
//   Variable<Slaves> slaves = variable.get();
//   slaves->add_infos()->MergeFrom(info);
//   Future<bool> set = state->set(&slaves);

// Forward declarations.
template <typename Serializer>
class State;
class ZooKeeperStateProcess;


template <typename T>
class Variable
{
public:
  T* operator -> ()
  {
    return &t;
  }

private:
  template <typename Serializer>
  friend class State; // Creates and manages variables.

  Variable(const Entry& _entry, const T& _t)
    : entry(_entry), t(_t)
  {}

  Entry entry; // Not const so Variable is copyable.
  T t;
};


template <typename Serializer = StringSerializer>
class State
{
public:
  State() {}
  virtual ~State() {}

  // Returns a variable from the state, creating a new one if one
  // previously did not exist (or an error if one occurs).
  template <typename T>
  process::Future<Variable<T> > get(const std::string& name);

  // Returns true if the variable was successfully set in the state,
  // otherwise false if the version of the variable was no longer
  // valid (or an error if one occurs).
  template <typename T>
  process::Future<bool> set(Variable<T>* variable);

  // Returns the collection of variable names in the state.
  virtual process::Future<std::vector<std::string> > names() = 0;

protected:
  // Fetch and swap state entries, factored out to allow State
  // implementations to be agnostic of Variable which is templated.
  virtual process::Future<Option<Entry> > fetch(const std::string& name) = 0;
  virtual process::Future<bool> swap(const Entry& entry, const UUID& uuid) = 0;

private:
  // Helper to convert an Entry into some Variable<T> (or create a
  // default Entry in the event no Entry was found). We make this a
  // static member of State for friend access to Variable's
  // constructor.
  template <typename T>
  static process::Future<Variable<T> > convert(
      const std::string& name,
      const Option<Entry>& option);

};


template <typename Serializer>
template <typename T>
process::Future<Variable<T> > State<Serializer>::convert(
    const std::string& name,
    const Option<Entry>& option)
{
  if (option.isSome()) {
    const Entry& entry = option.get();

    Try<T> t = Serializer::template deserialize<T>(entry.value());

    if (t.isError()) {
      return process::Future<Variable<T> >::failed(t.error());
    }

    return Variable<T>(entry, t.get());
  }

  // Otherwise, construct a Variable out of a new Entry with a default
  // value for T (and a random UUID to start).
  T t;

  Try<std::string> value = Serializer::template serialize<T>(t);

  if (value.isError()) {
    return process::Future<Variable<T> >::failed(value.error());
  }

  Entry entry;
  entry.set_name(name);
  entry.set_uuid(UUID::random().toBytes());
  entry.set_value(value.get());

  return Variable<T>(entry, t);
}


template <typename Serializer>
template <typename T>
process::Future<Variable<T> > State<Serializer>::get(const std::string& name)
{
  std::tr1::function<
  process::Future<Variable<T> >(const Option<Entry>&)> convert =
    std::tr1::bind(&State<Serializer>::template convert<T>,
                   name,
                   std::tr1::placeholders::_1);

  return fetch(name).then(convert);
}


template <typename Serializer>
template <typename T>
process::Future<bool> State<Serializer>::set(Variable<T>* variable)
{
  Try<std::string> value = Serializer::template serialize<T>(variable->t);

  if (value.isError()) {
    return process::Future<bool>::failed(value.error());
  }

  // Note that we try and swap an entry even if the value didn't change!
  UUID uuid = UUID::fromBytes(variable->entry.uuid());

  // Update the UUID and value of the entry.
  variable->entry.set_uuid(UUID::random().toBytes());
  variable->entry.set_value(value.get());

  return swap(variable->entry, uuid);
}

} // namespace state {
} // namespace internal {
} // namespace mesos {

#endif // __STATE_STATE_HPP__
