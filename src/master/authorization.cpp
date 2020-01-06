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

#include <stout/protobuf.hpp>
#include "common/protobuf_utils.hpp"

#include "master/authorization.hpp"

using std::ostream;


namespace mesos {
namespace authorization {


ActionObject ActionObject::taskLaunch(
    const TaskInfo& task,
    const FrameworkInfo& framework)
{
  Object object;
  *object.mutable_task_info() = task;
  *object.mutable_framework_info() = framework;

  return ActionObject(authorization::RUN_TASK, std::move(object));
}


ActionObject ActionObject::frameworkRegistration(
    const FrameworkInfo& frameworkInfo)
{
  Object object;
  *object.mutable_framework_info() = frameworkInfo;

  // For non-`MULTI_ROLE` frameworks, also propagate its single role
  // via the request's `value` field. This is purely for backwards
  // compatibility as the `value` field is deprecated. Note that this
  // means that authorizers relying on the deprecated field will see
  // an empty string in `value` for `MULTI_ROLE` frameworks.
  //
  // TODO(bbannier): Remove this at the end of `value`'s deprecation
  // cycle, see MESOS-7073.
  if (!::mesos::internal::protobuf::frameworkHasCapability(
          frameworkInfo, FrameworkInfo::Capability::MULTI_ROLE)) {
    object.set_value(frameworkInfo.role());
  }

  return ActionObject(authorization::REGISTER_FRAMEWORK, std::move(object));
}


ostream& operator<<(ostream& stream, const ActionObject& actionObject)
{
  const Option<Object>& object = actionObject.object();

  if (object.isNone()) {
    return stream << "perform action " << Action_Name(actionObject.action())
                  << " on ANY object";
  }

  switch (actionObject.action()) {
    case authorization::RUN_TASK:
      return stream
        << "launch task " << object->task_info().task_id()
        << " of framework " << object->framework_info().id();

    case authorization::REGISTER_FRAMEWORK:
      return stream
        << "register framework " << object->framework_info().id()
        << " with roles "
        << stringify(::mesos::internal::protobuf::framework::getRoles(
               object->framework_info()));

    default:
      break;
  }

  return stream
    << "perform action " << Action_Name(actionObject.action())
    << " on object " << jsonify(JSON::Protobuf(*object));
}


} // namespace authorization {
} // namespace mesos {
