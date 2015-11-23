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

#ifndef __STATE_PROTOBUF_HPP__
#define __STATE_PROTOBUF_HPP__

#include <string>

#include <process/future.hpp>

#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/some.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "messages/messages.hpp"
#include "messages/state.hpp"

#include "state/state.hpp"
#include "state/storage.hpp"

namespace mesos {
namespace internal {
namespace state {
namespace protobuf {

class State; // Forward declaration.


template <typename T>
class Variable
{
public:
  T get() const
  {
    return t;
  }

  Variable mutate(const T& t) const
  {
    Variable variable(*this);
    variable.t = t;
    return variable;
  }

private:
  friend class State; // Creates and manages variables.

  Variable(const state::Variable& _variable, const T& _t)
    : variable(_variable), t(_t)
  {}

  state::Variable variable; // Not const to keep Variable assignable.
  T t;
};


class State : public state::State
{
public:
  explicit State(Storage* storage) : state::State(storage) {}
  virtual ~State() {}

  // Returns a variable from the state, creating a new one if one
  // previously did not exist (or an error if one occurs).
  template <typename T>
  process::Future<Variable<T> > fetch(const std::string& name);

  // Returns the variable specified if it was successfully stored in
  // the state, otherwise returns none if the version of the variable
  // was no longer valid, or an error if one occurs.
  template <typename T>
  process::Future<Option<Variable<T> > > store(const Variable<T>& variable);

  // Expunges the variable from the state.
  template <typename T>
  process::Future<bool> expunge(const Variable<T>& variable);

private:
  // Helpers to handle future results from fetch and swap. We make
  // these static members of State for friend access to Variable's
  // constructor.
  template <typename T>
  static process::Future<Variable<T> > _fetch(
      const state::Variable& option);

  template <typename T>
  static process::Future<Option<Variable<T> > > _store(
      const T& t,
      const Option<state::Variable>& variable);
};


template <typename T>
process::Future<Variable<T> > State::fetch(const std::string& name)
{
  return state::State::fetch(name)
    .then(lambda::bind(&State::template _fetch<T>, lambda::_1));
}


template <typename T>
process::Future<Variable<T> > State::_fetch(
    const state::Variable& variable)
{
  Try<T> t = messages::deserialize<T>(variable.value());
  if (t.isError()) {
    return process::Failure(t.error());
  }

  return Variable<T>(variable, t.get());
}


template <typename T>
process::Future<Option<Variable<T> > > State::store(
    const Variable<T>& variable)
{
  Try<std::string> value = messages::serialize(variable.t);

  if (value.isError()) {
    return process::Failure(value.error());
  }

  return state::State::store(variable.variable.mutate(value.get()))
    .then(lambda::bind(&State::template _store<T>, variable.t, lambda::_1));
}


template <typename T>
process::Future<Option<Variable<T> > > State::_store(
    const T& t,
    const Option<state::Variable>& variable)
{
  if (variable.isSome()) {
    return Some(Variable<T>(variable.get(), t));
  }

  return None();
}


template <typename T>
process::Future<bool> State::expunge(const Variable<T>& variable)
{
  return state::State::expunge(variable.variable);
}

} // namespace protobuf {
} // namespace state {
} // namespace internal {
} // namespace mesos {

#endif // __STATE_PROTOBUF_HPP__
