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

#ifndef __STATE_HPP__
#define __STATE_HPP__

#include <google/protobuf/message.h>

#include <google/protobuf/io/zero_copy_stream_impl.h> // For ArrayInputStream.

#include <string>

#include <process/future.hpp>

#include "common/option.hpp"
#include "common/time.hpp"
#include "common/uuid.hpp"

#include "logging/logging.hpp"

#include "messages/state.hpp"

#include "zookeeper/authentication.hpp"

namespace mesos {
namespace internal {
namespace state {

// Forward declarations.
class LevelDBStateProcess;
class ZooKeeperStateProcess;

// An abstraction of "state" (possibly between multiple distributed
// components) represented by "variables" (effectively key/value
// pairs). The value of a variable in the state must be a protocol
// buffer. Variables are versioned such that setting a variable in the
// state will only succeed if the variable has not changed since last
// fetched. Varying implementations of state provide varying
// replicated guarantees.
//
// Note that the semantics of 'get' and 'set' provide atomicity. That
// is, you can not set a variable that has changed since you did the
// last get. That is, if a set succeeds then no other writes have been
// performed on the variable since your get.

// Example:
//   State* state = new ZooKeeperState();
//   Future<State::Variable<Slaves> > variable = state->get<Slaves>("slaves");
//   State::Variable<Slaves> slaves = variable.get();
//   slaves->add_infos()->MergeFrom(info);
//   Future<bool> set = state->set(&slaves);

class State
{
public:
  template <typename T>
  class Variable
  {
  public:
    T* operator -> ()
    {
      return &t;
    }

  private:
    friend class State; // Creates and manages variables.

    Variable(const Entry& _entry, const T& _t)
      : entry(_entry), t(_t)
    {
      const google::protobuf::Message* message = &t; // Check T is a protobuf.
    }

    Entry entry;
    T t;
  };

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

protected:
  // Fetch and swap state entries, factored out to allow State
  // implementations to be agnostic of Variable which is templated.
  virtual process::Future<Option<Entry> > fetch(const std::string& name) = 0;
  virtual process::Future<bool> swap(const Entry& entry, const UUID& uuid) = 0;

private:
  // Helper to convert an Entry to a Variable<T>. We make this a
  // static member of State for friend access to Variable's
  // constructor.
  template <typename T>
  static process::Future<State::Variable<T> > convert(
      const std::string& name,
      const Option<Entry>& option);
};


// Helper for converting an Entry into a Variable<T>.
template <typename T>
process::Future<State::Variable<T> > State::convert(
    const std::string& name,
    const Option<Entry>& option)
{
  T t;

  if (option.isSome()) {
    const Entry& entry = option.get();

    // TODO(benh): Check _compatibility_ versus equivalance.
    CHECK(t.GetDescriptor()->full_name() == entry.type());

    const std::string& value = entry.value();
    google::protobuf::io::ArrayInputStream stream(value.data(), value.size());
    if (!t.ParseFromZeroCopyStream(&stream)) {
      return process::Future<State::Variable<T> >::failed(
          "Failed to deserialize " + t.GetDescriptor()->full_name());
    }

    return State::Variable<T>(entry, t);
  }

  // Otherwise, construct a Variable out of a new Entry with a default
  // value for T (and a random UUID to start).
  std::string value;

  if (!t.SerializeToString(&value)) {
    return process::Future<State::Variable<T> >::failed(
        "Failed to serialize " + t.GetDescriptor()->full_name());
  }

  Entry entry;
  entry.set_name(name);
  entry.set_uuid(UUID::random().toBytes());
  entry.set_type(t.GetDescriptor()->full_name());
  entry.set_value(value);

  return State::Variable<T>(entry, t);
}


template <typename T>
process::Future<State::Variable<T> > State::get(const std::string& name)
{
  std::tr1::function<
  process::Future<State::Variable<T> >(const Option<Entry>&)> convert =
    std::tr1::bind(&State::convert<T>, name, std::tr1::placeholders::_1);

  return fetch(name).then(convert);
}


template <typename T>
process::Future<bool> State::set(State::Variable<T>* variable)
{
  std::string value;
  if (!variable->t.SerializeToString(&value)) {
    return process::Future<bool>::failed(
        "Failed to serialize " + variable->entry.type());
  }

  // Note that we try and swap an entry even if the value didn't change!
  UUID uuid = UUID::fromBytes(variable->entry.uuid());

  // Update the UUID and value of the entry.
  variable->entry.set_uuid(UUID::random().toBytes());
  variable->entry.set_value(value);

  return swap(variable->entry, uuid);
}


class LevelDBState : public State
{
public:
  LevelDBState(const std::string& path);
  virtual ~LevelDBState();

protected:
  // State implementation.
  virtual process::Future<Option<Entry> > fetch(const std::string& name);
  virtual process::Future<bool> swap(const Entry& entry, const UUID& uuid);

private:
  LevelDBStateProcess* process;
};


class ZooKeeperState : public State
{
public:
  // TODO(benh): Just take a zookeeper::URL.
  ZooKeeperState(
      const std::string& servers,
      const seconds& timeout,
      const std::string& znode,
      const Option<zookeeper::Authentication>& auth =
      Option<zookeeper::Authentication>());
  virtual ~ZooKeeperState();

protected:
  // State implementation.
  virtual process::Future<Option<Entry> > fetch(const std::string& name);
  virtual process::Future<bool> swap(const Entry& entry, const UUID& uuid);

private:
  ZooKeeperStateProcess* process;
};

} // namespace state {
} // namespace internal {
} // namespace mesos {

#endif // __STATE_HPP__




// need a Future::operator -> (), plus a test


// have a way to "watch" a Variable and get a future that signifies when/if it has changed?

// need a master work directory for local leveldb version of State!

// use leveldb for non-ha version of master (no reading/writing files)

// need to set the location of master detector zk znode, and set this znode location
