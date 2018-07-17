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

#ifndef __MESOS_STATE_PROTOBUF_HPP__
#define __MESOS_STATE_PROTOBUF_HPP__

#include <string>

#include <mesos/state/state.hpp>
#include <mesos/state/storage.hpp>

#include <process/future.hpp>

#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/some.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

namespace mesos {
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

  Variable(const mesos::state::Variable& _variable, const T& _t)
    : variable(_variable), t(_t)
  {}

  mesos::state::Variable variable; // Not const to keep Variable assignable.
  T t;
};


class State : public mesos::state::State
{
public:
  explicit State(mesos::state::Storage* storage)
    : mesos::state::State(storage) {}
  ~State() override {}

  // Returns a variable from the state, creating a new one if one
  // previously did not exist (or an error if one occurs).
  template <typename T>
  process::Future<Variable<T>> fetch(const std::string& name);

  // Returns the variable specified if it was successfully stored in
  // the state, otherwise returns none if the version of the variable
  // was no longer valid, or an error if one occurs.
  template <typename T>
  process::Future<Option<Variable<T>>> store(const Variable<T>& variable);

  // Expunges the variable from the state.
  template <typename T>
  process::Future<bool> expunge(const Variable<T>& variable);

private:
  // Helpers to handle future results from fetch and swap. We make
  // these static members of State for friend access to Variable's
  // constructor.
  template <typename T>
  static process::Future<Variable<T>> _fetch(
      const mesos::state::Variable& variable);

  template <typename T>
  static process::Future<Option<Variable<T>>> _store(
      const T& t,
      const Option<mesos::state::Variable>& variable);
};


template <typename T>
process::Future<Variable<T>> State::fetch(const std::string& name)
{
  return mesos::state::State::fetch(name)
    .then(lambda::bind(&State::template _fetch<T>, lambda::_1));
}


template <typename T>
process::Future<Variable<T>> State::_fetch(
    const mesos::state::Variable& variable)
{
  Try<T> t = ::protobuf::deserialize<T>(variable.value());
  if (t.isError()) {
    return process::Failure(t.error());
  }

  return Variable<T>(variable, t.get());
}


template <typename T>
process::Future<Option<Variable<T>>> State::store(
    const Variable<T>& variable)
{
  Try<std::string> value = ::protobuf::serialize(variable.t);

  if (value.isError()) {
    return process::Failure(value.error());
  }

  return mesos::state::State::store(variable.variable.mutate(value.get()))
    .then(lambda::bind(&State::template _store<T>, variable.t, lambda::_1));
}


template <typename T>
process::Future<Option<Variable<T>>> State::_store(
    const T& t,
    const Option<mesos::state::Variable>& variable)
{
  if (variable.isSome()) {
    return Some(Variable<T>(variable.get(), t));
  }

  return None();
}


template <typename T>
process::Future<bool> State::expunge(const Variable<T>& variable)
{
  return mesos::state::State::expunge(variable.variable);
}

} // namespace protobuf {
} // namespace state {
} // namespace mesos {

#endif // __MESOS_STATE_PROTOBUF_HPP__
