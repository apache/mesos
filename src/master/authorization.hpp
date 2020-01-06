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

#ifndef __MASTER_AUTHORIZATION_HPP__
#define __MASTER_AUTHORIZATION_HPP__

#include <ostream>

#include <stout/option.hpp>

#include <mesos/mesos.pb.h>
#include <mesos/authorizer/authorizer.pb.h>

namespace mesos {
namespace authorization {


class ActionObject
{
public:
  Action action() const { return action_; }

  const Option<Object>& object() const& { return object_; }
  Option<Object>&& object() && { return std::move(object_); }

  // Returns action-object pair for authorizing a task launch.
  static ActionObject taskLaunch(
      const TaskInfo& task,
      const FrameworkInfo& framework);

  // Returns action-object pair for authorizing
  // framework (re)registration or update.
  static ActionObject frameworkRegistration(
      const FrameworkInfo& frameworkInfo);

private:
  Action action_;
  Option<Object> object_;

  ActionObject(Action action, Option<Object>&& object)
    : action_(action), object_(object){};
};

// Outputs human-readable description of an ActionObject.
//
// NOTE: For more convenient use in authorization-related messages
// ("Authorizing principal 'baz' to launch task", "Forbidden to create disk...",
// and so on), the description starts with a verb.
//
// Output examples:
//  - "launch task 123 of framework de-adbe-ef",
//  - "perform action FOO_BAR on object '{task_info: ..., machine_id: ...}"
std::ostream& operator<<(
    std::ostream& stream,
    const ActionObject& actionObject);


} // namespace authorization {
} // namespace mesos {

#endif // __MASTER_AUTHORIZATION_HPP__
