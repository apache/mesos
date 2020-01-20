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

#include "authorizer/local/authorizer.hpp"

#include <memory>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <mesos/authorizer/acls.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include "common/authorization.hpp"
#include "common/http.hpp"
#include "common/parse.hpp"
#include "common/protobuf_utils.hpp"

using std::shared_ptr;
using std::string;
using std::vector;

using process::Failure;
using process::Future;

using process::dispatch;

namespace mesos {
namespace internal {

struct GenericACL
{
  ACL::Entity subjects;
  ACL::Entity objects;
};


// Match matrix:
//
//                  -----------ACL----------
//
//                    SOME    NONE    ANY
//          -------|-------|-------|-------
//  |        SOME  | Yes/No|  Yes  |   Yes
//  |       -------|-------|-------|-------
// Request   NONE  |  No   |  Yes  |   No
//  |       -------|-------|-------|-------
//  |        ANY   |  No   |  Yes  |   Yes
//          -------|-------|-------|-------
static bool matches(const ACL::Entity& request, const ACL::Entity& acl)
{
  // NONE only matches with NONE.
  if (request.type() == ACL::Entity::NONE) {
    return acl.type() == ACL::Entity::NONE;
  }

  // ANY matches with ANY or NONE.
  if (request.type() == ACL::Entity::ANY) {
    return acl.type() == ACL::Entity::ANY || acl.type() == ACL::Entity::NONE;
  }

  if (request.type() == ACL::Entity::SOME) {
    // SOME matches with ANY or NONE.
    if (acl.type() == ACL::Entity::ANY || acl.type() == ACL::Entity::NONE) {
      return true;
    }

    // SOME is allowed if the request values are a subset of ACL
    // values.
    foreach (const string& value, request.values()) {
      bool found = false;
      foreach (const string& value_, acl.values()) {
        if (value == value_) {
          found = true;
          break;
        }
      }

      if (!found) {
        return false;
      }
    }
    return true;
  }

  return false;
}

// Allow matrix:
//
//                 -----------ACL----------
//
//                    SOME    NONE    ANY
//          -------|-------|-------|-------
//  |        SOME  | Yes/No|  No   |   Yes
//  |       -------|-------|-------|-------
// Request   NONE  |  No   |  Yes  |   No
//  |       -------|-------|-------|-------
//  |        ANY   |  No   |  No   |   Yes
//          -------|-------|-------|-------
static bool allows(const ACL::Entity& request, const ACL::Entity& acl)
{
  // NONE is only allowed by NONE.
  if (request.type() == ACL::Entity::NONE) {
    return acl.type() == ACL::Entity::NONE;
  }

  // ANY is only allowed by ANY.
  if (request.type() == ACL::Entity::ANY) {
    return acl.type() == ACL::Entity::ANY;
  }

  if (request.type() == ACL::Entity::SOME) {
    // SOME is allowed by ANY.
    if (acl.type() == ACL::Entity::ANY) {
      return true;
    }

    // SOME is not allowed by NONE.
    if (acl.type() == ACL::Entity::NONE) {
      return false;
    }

    // SOME is allowed if the request values are a subset of ACL
    // values.
    foreach (const string& value, request.values()) {
      bool found = false;
      foreach (const string& value_, acl.values()) {
        if (value == value_) {
          found = true;
          break;
        }
      }

      if (!found) {
        return false;
      }
    }
    return true;
  }

  return false;
}


class LocalAuthorizerObjectApprover : public ObjectApprover
{
public:
  LocalAuthorizerObjectApprover(
      const vector<GenericACL>& acls,
      const Option<authorization::Subject>& subject,
      const authorization::Action& action,
      bool permissive)
    : acls_(acls),
      subject_(subject),
      action_(action),
      permissive_(permissive) {}

  Try<bool> approved(
      const Option<ObjectApprover::Object>& object) const noexcept override
  {
    // Construct subject.
    ACL::Entity aclSubject;
    if (subject_.isSome()) {
      aclSubject.add_values(subject_->value());
      aclSubject.set_type(mesos::ACL::Entity::SOME);
    } else {
      aclSubject.set_type(mesos::ACL::Entity::ANY);
    }

    // Construct object.
    ACL::Entity aclObject;

    if (object.isNone()) {
      aclObject.set_type(mesos::ACL::Entity::ANY);
    } else {
      switch (action_) {
        case authorization::GET_ENDPOINT_WITH_PATH:
          // Check object has the required types set.
          CHECK_NOTNULL(object->value);

          aclObject.add_values(*(object->value));
          aclObject.set_type(mesos::ACL::Entity::SOME);

          break;
        case authorization::TEARDOWN_FRAMEWORK:
          aclObject.set_type(mesos::ACL::Entity::SOME);
          if (object->framework_info) {
            aclObject.add_values(object->framework_info->principal());
          } else if (object->value) {
            aclObject.add_values(*(object->value));
          } else {
            aclObject.set_type(mesos::ACL::Entity::ANY);
          }

          break;
        case authorization::DESTROY_VOLUME:
          aclObject.set_type(mesos::ACL::Entity::SOME);
          if (object->resource) {
            aclObject.add_values(
                object->resource->disk().persistence().principal());
          } else if (object->value) {
            aclObject.add_values(*(object->value));
          } else {
            aclObject.set_type(mesos::ACL::Entity::ANY);
          }

          break;
        case authorization::UNRESERVE_RESOURCES:
          aclObject.set_type(mesos::ACL::Entity::SOME);
          if (object->resource) {
            if (object->resource->reservations_size() > 0) {
              // Check for principal in "post-reservation-refinement" format.
              aclObject.add_values(
                  object->resource->reservations().rbegin()->principal());
            } else {
              // Check for principal in "pre-reservation-refinement" format.
              aclObject.add_values(
                  object->resource->reservation().principal());
            }
          } else if (object->value) {
            aclObject.add_values(*(object->value));
          } else {
            aclObject.set_type(mesos::ACL::Entity::ANY);
          }

          break;
        case authorization::RUN_TASK:
          aclObject.set_type(mesos::ACL::Entity::SOME);
          if (object->task_info && object->task_info->has_command() &&
              object->task_info->command().has_user()) {
            aclObject.add_values(object->task_info->command().user());
          } else if (object->task_info && object->task_info->has_executor() &&
              object->task_info->executor().command().has_user()) {
            aclObject.add_values(
                object->task_info->executor().command().user());
          } else if (object->framework_info) {
            aclObject.add_values(object->framework_info->user());
          } else {
            aclObject.set_type(mesos::ACL::Entity::ANY);
          }

          break;
        case authorization::ACCESS_MESOS_LOG:
          aclObject.set_type(mesos::ACL::Entity::ANY);

          break;
        case authorization::VIEW_FLAGS:
          aclObject.set_type(mesos::ACL::Entity::ANY);

          break;
        case authorization::ATTACH_CONTAINER_INPUT:
        case authorization::ATTACH_CONTAINER_OUTPUT:
        case authorization::REMOVE_NESTED_CONTAINER:
        case authorization::KILL_NESTED_CONTAINER:
        case authorization::WAIT_NESTED_CONTAINER:
          aclObject.set_type(mesos::ACL::Entity::ANY);

          if (object->executor_info != nullptr &&
              object->executor_info->command().has_user()) {
            aclObject.add_values(object->executor_info->command().user());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          } else if (object->framework_info != nullptr &&
                     object->framework_info->has_user()) {
            aclObject.add_values(object->framework_info->user());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          } else if (object->container_id != nullptr) {
            aclObject.add_values(object->container_id->value());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          }

          break;
        case authorization::ACCESS_SANDBOX:
          aclObject.set_type(mesos::ACL::Entity::ANY);

          if (object->executor_info != nullptr &&
              object->executor_info->command().has_user()) {
            aclObject.add_values(object->executor_info->command().user());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          } else if (object->framework_info != nullptr) {
            aclObject.add_values(object->framework_info->user());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          }

          break;
        case authorization::VIEW_FRAMEWORK:
          // Check object has the required types set.
          CHECK_NOTNULL(object->framework_info);

          aclObject.add_values(object->framework_info->user());
          aclObject.set_type(mesos::ACL::Entity::SOME);

          break;
        case authorization::VIEW_TASK: {
          CHECK(object->task != nullptr || object->task_info != nullptr);
          CHECK_NOTNULL(object->framework_info);

          // First we consider either whether `Task` or `TaskInfo`
          // have `user` set. As fallback we use `FrameworkInfo.user`.
          Option<string> taskUser = None();
          if (object->task != nullptr && object->task->has_user()) {
            taskUser = object->task->user();
          } else if (object->task_info != nullptr) {
            // Within TaskInfo the user can be either set in `command`
            // or `executor.command`.
            if (object->task_info->has_command() &&
                object->task_info->command().has_user()) {
              taskUser = object->task_info->command().user();
            } else if (object->task_info->has_executor() &&
                       object->task_info->executor().command().has_user()) {
              taskUser = object->task_info->executor().command().user();
            }
          }

          // In case there is no `user` set on task level we fallback
          // to the `FrameworkInfo.user`.
          if (taskUser.isNone()) {
            taskUser = object->framework_info->user();
          }
          aclObject.add_values(taskUser.get());
          aclObject.set_type(mesos::ACL::Entity::SOME);

          break;
        }
        case authorization::VIEW_EXECUTOR:
          CHECK_NOTNULL(object->executor_info);
          CHECK_NOTNULL(object->framework_info);

          if (object->executor_info->command().has_user()) {
            aclObject.add_values(object->executor_info->command().user());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          } else {
            aclObject.add_values(object->framework_info->user());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          }

          break;
        case authorization::LAUNCH_NESTED_CONTAINER:
        case authorization::LAUNCH_NESTED_CONTAINER_SESSION:
          aclObject.set_type(mesos::ACL::Entity::ANY);

          if (object->command_info != nullptr) {
            if (object->command_info->has_user()) {
              aclObject.add_values(object->command_info->user());
              aclObject.set_type(mesos::ACL::Entity::SOME);
            }
            break;
          }

          if (object->executor_info != nullptr &&
              object->executor_info->command().has_user()) {
            aclObject.add_values(object->executor_info->command().user());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          } else if (object->framework_info != nullptr &&
              object->framework_info->has_user()) {
            aclObject.add_values(object->framework_info->user());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          }

          break;
        case authorization::VIEW_CONTAINER:
          aclObject.set_type(mesos::ACL::Entity::ANY);

          if (object->executor_info != nullptr &&
              object->executor_info->command().has_user()) {
            aclObject.add_values(object->executor_info->command().user());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          } else if (object->framework_info != nullptr &&
              object->framework_info->has_user()) {
            aclObject.add_values(object->framework_info->user());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          }

          break;
        case authorization::LAUNCH_STANDALONE_CONTAINER:
        case authorization::KILL_STANDALONE_CONTAINER:
        case authorization::WAIT_STANDALONE_CONTAINER:
        case authorization::REMOVE_STANDALONE_CONTAINER:
        case authorization::VIEW_STANDALONE_CONTAINER:
        case authorization::GET_MAINTENANCE_SCHEDULE:
        case authorization::GET_MAINTENANCE_STATUS:
        case authorization::MARK_AGENT_GONE:
        case authorization::REGISTER_AGENT:
        case authorization::SET_LOG_LEVEL:
        case authorization::START_MAINTENANCE:
        case authorization::STOP_MAINTENANCE:
        case authorization::UPDATE_MAINTENANCE_SCHEDULE:
        case authorization::DRAIN_AGENT:
        case authorization::DEACTIVATE_AGENT:
        case authorization::REACTIVATE_AGENT:
        case authorization::MODIFY_RESOURCE_PROVIDER_CONFIG:
        case authorization::MARK_RESOURCE_PROVIDER_GONE:
        case authorization::VIEW_RESOURCE_PROVIDER:
        case authorization::PRUNE_IMAGES:
          aclObject.set_type(ACL::Entity::ANY);

          break;
        case authorization::CREATE_VOLUME:
        case authorization::RESIZE_VOLUME:
        case authorization::GET_QUOTA:
        case authorization::RESERVE_RESOURCES:
        case authorization::UPDATE_QUOTA:
        case authorization::UPDATE_QUOTA_WITH_CONFIG:
        case authorization::UPDATE_WEIGHT:
        case authorization::VIEW_ROLE:
        case authorization::REGISTER_FRAMEWORK:
        case authorization::CREATE_BLOCK_DISK:
        case authorization::DESTROY_BLOCK_DISK:
        case authorization::CREATE_MOUNT_DISK:
        case authorization::DESTROY_MOUNT_DISK:
        case authorization::DESTROY_RAW_DISK:
          return Error("Authorization for action " + stringify(action_) +
                       " requires a specialized approver object.");
        case authorization::UNKNOWN:
          LOG(WARNING) << "Authorization for action '" << action_
                       << "' is not defined and therefore not authorized";
          return false;
      }
    }

    return approved(acls_, aclSubject, aclObject);
  }

private:
  bool approved(
      const vector<GenericACL>& acls,
      const ACL::Entity& subject,
      const ACL::Entity& object) const
  {
    // Authorize subject/object.
    foreach (const GenericACL& acl, acls) {
      if (matches(subject, acl.subjects) && matches(object, acl.objects)) {
        return allows(subject, acl.subjects) && allows(object, acl.objects);
      }
    }

    return permissive_; // None of the ACLs match.
  }

  const vector<GenericACL> acls_;
  const Option<authorization::Subject> subject_;
  const authorization::Action action_;
  const bool permissive_;
};


class LocalNestedContainerObjectApprover : public ObjectApprover
{
public:
  LocalNestedContainerObjectApprover(
      const vector<GenericACL>& userAcls,
      const vector<GenericACL>& parentAcls,
      const Option<authorization::Subject>& subject,
      const authorization::Action& action,
      bool permissive)
    : childApprover_(userAcls, subject, action, permissive),
      parentApprover_(parentAcls, subject, action, permissive) {}

  // Launching Nested Containers and sessions in Nester Containers is
  // authorized if a principal is allowed to launch nester container (sessions)
  // under an executor running under a given OS user and, if a command
  // is available, the principal is also allowed to run the command as
  // the given OS user.
  Try<bool> approved(
      const Option<ObjectApprover::Object>& object) const noexcept override
  {
    if (object.isNone() || object->command_info == nullptr) {
      return parentApprover_.approved(object);
    }

    ObjectApprover::Object parentObject;
    parentObject.executor_info = object->executor_info;
    parentObject.framework_info = object->framework_info;

    Try<bool> parentApproved = parentApprover_.approved(parentObject);

    if (parentApproved.isError()) {
      return parentApproved;
    }

    ObjectApprover::Object childObject;
    childObject.command_info = object->command_info;

    Try<bool> childApproved = childApprover_.approved(childObject);

    if (childApproved.isError()) {
      return childApproved;
    }

    return parentApproved.get() && childApproved.get();
  }

private:
  LocalAuthorizerObjectApprover childApprover_;
  LocalAuthorizerObjectApprover parentApprover_;
};


class LocalImplicitExecutorObjectApprover : public ObjectApprover
{
public:
  LocalImplicitExecutorObjectApprover(const ContainerID& subject)
    : subject_(subject) {}

  // Executors are permitted to perform an action when the root ContainerID in
  // the object is equal to the ContainerID that was extracted from the
  // subject's claims.
  Try<bool> approved(
      const Option<ObjectApprover::Object>& object) const noexcept override
  {
    return object.isSome() &&
           object->container_id != nullptr &&
           subject_ == protobuf::getRootContainerId(*object->container_id);
  }

private:
  const ContainerID subject_;
};


class LocalImplicitResourceProviderObjectApprover : public ObjectApprover
{
public:
  LocalImplicitResourceProviderObjectApprover(const string& subject)
    : subject_(subject) {}

  // Resource providers are permitted to perform an action when the
  // ContainerID in the object is prefixed by the namespace extracted
  // from the subject's claims.
  Try<bool> approved(
      const Option<ObjectApprover::Object>& object) const noexcept override
  {
    return object.isSome() &&
           object->container_id != nullptr &&
           strings::startsWith(object->container_id->value(), subject_);
  }

private:
  const string subject_;
};


// Implementation of the ObjectApprover interface denying all objects.
class RejectingObjectApprover : public ObjectApprover
{
public:
  Try<bool> approved(
      const Option<ObjectApprover::Object>& object) const noexcept override
  {
    return false;
  }
};


class LocalHierarchicalRoleApprover : public ObjectApprover
{
public:
  LocalHierarchicalRoleApprover(
      const vector<GenericACL>& acls,
      const Option<authorization::Subject>& subject,
      const authorization::Action& action,
      bool permissive)
    : acls_(acls), subject_(subject), action_(action), permissive_(permissive)
  {
    if (subject_.isSome()) {
      entitySubject_.set_type(ACL::Entity::SOME);
      entitySubject_.add_values(subject_->value());
    } else {
      entitySubject_.set_type(ACL::Entity::ANY);
    }
  }

  Try<bool> approved(const Option<ObjectApprover::Object>& object) const
      noexcept override
  {
    ACL::Entity entityObject;

    if (object.isNone()) {
      entityObject.set_type(ACL::Entity::ANY);
    } else {
      switch (action_) {
        case authorization::CREATE_VOLUME:
        case authorization::RESIZE_VOLUME:
        case authorization::RESERVE_RESOURCES:
        case authorization::CREATE_BLOCK_DISK:
        case authorization::DESTROY_BLOCK_DISK:
        case authorization::CREATE_MOUNT_DISK:
        case authorization::DESTROY_MOUNT_DISK:
        case authorization::DESTROY_RAW_DISK: {
          entityObject.set_type(ACL::Entity::SOME);
          if (object->resource) {
            if (object->resource->reservations_size() > 0) {
              // Check for role in "post-reservation-refinement" format.
              entityObject.add_values(
                  object->resource->reservations().rbegin()->role());
            } else {
              // Check for role in "pre-reservation-refinement" format.
              entityObject.add_values(object->resource->role());
            }
          } else if (object->value) {
            entityObject.add_values(*(object->value));
          } else {
            entityObject.set_type(ACL::Entity::ANY);
          }
          break;
        }
        case authorization::UPDATE_WEIGHT: {
          entityObject.set_type(mesos::ACL::Entity::SOME);
          if (object->weight_info) {
            entityObject.add_values(object->weight_info->role());
          } else if (object->value) {
            entityObject.add_values(*(object->value));
          } else {
            entityObject.set_type(mesos::ACL::Entity::ANY);
          }

          break;
        }
        case authorization::VIEW_ROLE: {
          // Check object has the required types set.
          CHECK_NOTNULL(object->value);

          entityObject.add_values(*(object->value));
          entityObject.set_type(mesos::ACL::Entity::SOME);

          break;
        }
        case authorization::GET_QUOTA: {
          entityObject.set_type(mesos::ACL::Entity::SOME);
          if (object->value) {
            entityObject.add_values(*(object->value));
          } else {
            entityObject.set_type(mesos::ACL::Entity::ANY);
          }

          break;
        }
        case authorization::UPDATE_QUOTA: {
          // Check object has the required types set.
          CHECK_NOTNULL(object->quota_info);

          entityObject.add_values(object->quota_info->role());
          entityObject.set_type(mesos::ACL::Entity::SOME);

          break;
        }
        case authorization::UPDATE_QUOTA_WITH_CONFIG: {
          // Check object has the required types set.
          CHECK_NOTNULL(object->value);

          entityObject.add_values(*(object->value));
          entityObject.set_type(mesos::ACL::Entity::SOME);

          break;
        }
        case authorization::REGISTER_FRAMEWORK: {
          vector<ACL::Entity> objects;
          if (object->framework_info) {
            foreach (
                const string& role,
                protobuf::framework::getRoles(*(object->framework_info))) {
              objects.emplace_back();
              objects.back().set_type(mesos::ACL::Entity::SOME);
              objects.back().add_values(role);
            }
          } else if (object->value) {
            // We also update the deprecated `value` field to support custom
            // authorizers not yet modified to examine `framework_info`.
            //
            // TODO(bbannier): Clean up use of `value` here, see MESOS-7091.
            objects.emplace_back();
            objects.back().set_type(mesos::ACL::Entity::SOME);
            objects.back().add_values(*(object->value));
          } else {
            objects.emplace_back();
            objects.back().set_type(mesos::ACL::Entity::ANY);
          }

          // The framework needs to be allowed to register under
          // all the roles it requests.
          foreach (const ACL::Entity& entity, objects) {
            if (!approved(acls_, entitySubject_, entity)) {
              return false;
            }
          }

          return objects.empty() ? permissive_ : true;
        }
        case authorization::ACCESS_MESOS_LOG:
        case authorization::ACCESS_SANDBOX:
        case authorization::ATTACH_CONTAINER_INPUT:
        case authorization::ATTACH_CONTAINER_OUTPUT:
        case authorization::DESTROY_VOLUME:
        case authorization::GET_ENDPOINT_WITH_PATH:
        case authorization::GET_MAINTENANCE_SCHEDULE:
        case authorization::GET_MAINTENANCE_STATUS:
        case authorization::KILL_NESTED_CONTAINER:
        case authorization::KILL_STANDALONE_CONTAINER:
        case authorization::LAUNCH_NESTED_CONTAINER:
        case authorization::LAUNCH_NESTED_CONTAINER_SESSION:
        case authorization::LAUNCH_STANDALONE_CONTAINER:
        case authorization::MARK_AGENT_GONE:
        case authorization::PRUNE_IMAGES:
        case authorization::REGISTER_AGENT:
        case authorization::REMOVE_NESTED_CONTAINER:
        case authorization::REMOVE_STANDALONE_CONTAINER:
        case authorization::RUN_TASK:
        case authorization::SET_LOG_LEVEL:
        case authorization::START_MAINTENANCE:
        case authorization::STOP_MAINTENANCE:
        case authorization::DRAIN_AGENT:
        case authorization::DEACTIVATE_AGENT:
        case authorization::REACTIVATE_AGENT:
        case authorization::TEARDOWN_FRAMEWORK:
        case authorization::UNRESERVE_RESOURCES:
        case authorization::UPDATE_MAINTENANCE_SCHEDULE:
        case authorization::VIEW_CONTAINER:
        case authorization::VIEW_EXECUTOR:
        case authorization::VIEW_FLAGS:
        case authorization::VIEW_FRAMEWORK:
        case authorization::VIEW_STANDALONE_CONTAINER:
        case authorization::VIEW_TASK:
        case authorization::WAIT_NESTED_CONTAINER:
        case authorization::WAIT_STANDALONE_CONTAINER:
        case authorization::MODIFY_RESOURCE_PROVIDER_CONFIG:
        case authorization::MARK_RESOURCE_PROVIDER_GONE:
        case authorization::VIEW_RESOURCE_PROVIDER:
        case authorization::UNKNOWN:
          UNREACHABLE();
      }
    }

    CHECK(
        entityObject.type() == ACL::Entity::ANY ||
        entityObject.values_size() == 1);

    return approved(acls_, entitySubject_, entityObject);
  }

private:
  bool approved(
      const vector<GenericACL>& acls,
      const ACL::Entity& subject,
      const ACL::Entity& object) const
  {
    // This entity is used for recursive hierarchies where we already
    // validated that the object role is a nested hierarchy of the
    // acl role.
    ACL::Entity aclAny;
    aclAny.set_type(ACL::Entity::ANY);

    foreach (const GenericACL& acl, acls) {
      if (!isRecursiveACL(acl)) {
        // If `acl` is not recursive, treat it as a normal acl.
        if (matches(subject, acl.subjects) && matches(object, acl.objects)) {
          return allows(subject, acl.subjects) && allows(object, acl.objects);
        }
      } else if (object.type() == ACL::Entity::SOME &&
          isNestedHierarchy(acl.objects.values(0), object.values(0))) {
        // Partial validation was done when verifying that the object is
        // a nested hierarchy.
        if (matches(subject, acl.subjects) && matches(object, aclAny)) {
          return allows(subject, acl.subjects) && allows(object, aclAny);
        }
      }
    }

    return permissive_;
  }

  static bool isRecursiveACL(const GenericACL& acl)
  {
    return acl.objects.values_size() == 1 &&
           strings::endsWith(acl.objects.values(0), "/%");
  }

  // Returns true if child is a nested hierarchy of parent, i.e. child has more
  // levels of nesting and all the levels of parent are a prefix of the levels
  // of child.
  static bool isNestedHierarchy(const string& parent, const string& child)
  {
    // Requires that parent ends with `/%`.
    CHECK(strings::endsWith(parent, "/%"));
    return strings::startsWith(child, parent.substr(0, parent.size() - 1));
  }

  vector<GenericACL> acls_;
  Option<authorization::Subject> subject_;
  authorization::Action action_;
  bool permissive_;
  ACL::Entity entitySubject_;
};


class LocalAuthorizerProcess : public ProtobufProcess<LocalAuthorizerProcess>
{
public:
  LocalAuthorizerProcess(const ACLs& _acls)
    : ProcessBase(process::ID::generate("local-authorizer")), acls(_acls) {}

  Future<bool> authorized(const authorization::Request& request)
  {
    Option<authorization::Subject> subject;
    if (request.has_subject()) {
      subject = request.subject();
    }

    Option<ObjectApprover::Object> object = None();
    if (request.has_object()) {
      object = ObjectApprover::Object(request.object());
    }

    const Try<shared_ptr<const ObjectApprover>> approver =
      getApprover(subject, request.action());

    if (approver.isError()) {
      return Failure(approver.error());
    }

    Try<bool> result = (*approver)->approved(object);
    if (result.isError()) {
      return Failure(result.error());
    }

    return result.get();
  }

  template <typename SomeACL>
  static vector<GenericACL> createHierarchicalRoleACLs(
      SomeACL&& someACL)
  {
    vector<GenericACL> acls;

    // This may split an ACL into several. This does not change semantics
    // since the split rules appear consecutively and if an object
    // matches any of the split ACLs in any order, it will yield the same
    // results.
    foreach (auto&& acl, someACL) {
      switch (acl.roles().type()) {
        case ACL::Entity::SOME: {
          ACL::Entity roles;
          foreach (const string& value, acl.roles().values()) {
            if (strings::endsWith(value, "/%")) {
              // Recursive ACLs only have one value in their object list.
              GenericACL acl_;
              acl_.subjects = acl.principals();
              acl_.objects.add_values(value);
              acls.push_back(acl_);
            } else {
              roles.add_values(value);
            }
          }

          if (!roles.values().empty()) {
            GenericACL acl_;
            acl_.subjects = acl.principals();
            acl_.objects = roles;
            acls.push_back(acl_);
          }
          break;
        }
        case ACL::Entity::ANY:
        case ACL::Entity::NONE: {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();
          acls.push_back(acl_);
          break;
        }
      }
    }

    return acls;
  }

  shared_ptr<const ObjectApprover> getHierarchicalRoleApprover(
      const Option<authorization::Subject>& subject,
      const authorization::Action& action) const
  {
    vector<GenericACL> hierarchicalRoleACLs;
    switch (action) {
      case authorization::CREATE_VOLUME: {
        hierarchicalRoleACLs =
            createHierarchicalRoleACLs(acls.create_volumes());
        break;
      }
      case authorization::RESIZE_VOLUME: {
        hierarchicalRoleACLs =
            createHierarchicalRoleACLs(acls.resize_volumes());
        break;
      }
      case authorization::RESERVE_RESOURCES: {
        hierarchicalRoleACLs =
            createHierarchicalRoleACLs(acls.reserve_resources());
        break;
      }
      case authorization::UPDATE_WEIGHT: {
        hierarchicalRoleACLs =
            createHierarchicalRoleACLs(acls.update_weights());
        break;
      }
      case authorization::VIEW_ROLE: {
        hierarchicalRoleACLs =
            createHierarchicalRoleACLs(acls.view_roles());
        break;
      }
      case authorization::GET_QUOTA: {
        hierarchicalRoleACLs =
            createHierarchicalRoleACLs(acls.get_quotas());
        break;
      }
      case authorization::REGISTER_FRAMEWORK: {
        hierarchicalRoleACLs =
            createHierarchicalRoleACLs(acls.register_frameworks());
        break;
      }
      case authorization::UPDATE_QUOTA:
      case authorization::UPDATE_QUOTA_WITH_CONFIG: {
        hierarchicalRoleACLs =
            createHierarchicalRoleACLs(acls.update_quotas());
        break;
      }
      case authorization::CREATE_BLOCK_DISK: {
        hierarchicalRoleACLs =
          createHierarchicalRoleACLs(acls.create_block_disks());
        break;
      }
      case authorization::DESTROY_BLOCK_DISK: {
        hierarchicalRoleACLs =
          createHierarchicalRoleACLs(acls.destroy_block_disks());
        break;
      }
      case authorization::CREATE_MOUNT_DISK: {
        hierarchicalRoleACLs =
          createHierarchicalRoleACLs(acls.create_mount_disks());
        break;
      }
      case authorization::DESTROY_MOUNT_DISK: {
        hierarchicalRoleACLs =
          createHierarchicalRoleACLs(acls.destroy_mount_disks());
        break;
      }
      case authorization::DESTROY_RAW_DISK: {
        hierarchicalRoleACLs =
          createHierarchicalRoleACLs(acls.destroy_raw_disks());
        break;
      }
      case authorization::ACCESS_MESOS_LOG:
      case authorization::ACCESS_SANDBOX:
      case authorization::ATTACH_CONTAINER_INPUT:
      case authorization::ATTACH_CONTAINER_OUTPUT:
      case authorization::DESTROY_VOLUME:
      case authorization::GET_ENDPOINT_WITH_PATH:
      case authorization::GET_MAINTENANCE_SCHEDULE:
      case authorization::GET_MAINTENANCE_STATUS:
      case authorization::KILL_NESTED_CONTAINER:
      case authorization::KILL_STANDALONE_CONTAINER:
      case authorization::LAUNCH_NESTED_CONTAINER:
      case authorization::LAUNCH_NESTED_CONTAINER_SESSION:
      case authorization::LAUNCH_STANDALONE_CONTAINER:
      case authorization::MARK_AGENT_GONE:
      case authorization::PRUNE_IMAGES:
      case authorization::REGISTER_AGENT:
      case authorization::REMOVE_NESTED_CONTAINER:
      case authorization::REMOVE_STANDALONE_CONTAINER:
      case authorization::RUN_TASK:
      case authorization::SET_LOG_LEVEL:
      case authorization::START_MAINTENANCE:
      case authorization::STOP_MAINTENANCE:
      case authorization::DRAIN_AGENT:
      case authorization::DEACTIVATE_AGENT:
      case authorization::REACTIVATE_AGENT:
      case authorization::TEARDOWN_FRAMEWORK:
      case authorization::UNKNOWN:
      case authorization::UNRESERVE_RESOURCES:
      case authorization::UPDATE_MAINTENANCE_SCHEDULE:
      case authorization::VIEW_CONTAINER:
      case authorization::VIEW_EXECUTOR:
      case authorization::VIEW_FLAGS:
      case authorization::VIEW_FRAMEWORK:
      case authorization::VIEW_STANDALONE_CONTAINER:
      case authorization::VIEW_TASK:
      case authorization::WAIT_NESTED_CONTAINER:
      case authorization::WAIT_STANDALONE_CONTAINER:
      case authorization::MODIFY_RESOURCE_PROVIDER_CONFIG:
      case authorization::MARK_RESOURCE_PROVIDER_GONE:
      case authorization::VIEW_RESOURCE_PROVIDER:
        UNREACHABLE();
    }

    return std::make_shared<LocalHierarchicalRoleApprover>(
        hierarchicalRoleACLs, subject, action, acls.permissive());
  }

  shared_ptr<const ObjectApprover> getNestedContainerObjectApprover(
      const Option<authorization::Subject>& subject,
      const authorization::Action& action) const
  {
    CHECK(action == authorization::LAUNCH_NESTED_CONTAINER ||
          action == authorization::LAUNCH_NESTED_CONTAINER_SESSION);

    vector<GenericACL> runAsUserAcls;
    vector<GenericACL> parentRunningAsUserAcls;

    if (action == authorization::LAUNCH_NESTED_CONTAINER) {
      foreach (const ACL::LaunchNestedContainerAsUser& acl,
               acls.launch_nested_containers_as_user()) {
        GenericACL acl_;
        acl_.subjects = acl.principals();
        acl_.objects = acl.users();

        runAsUserAcls.push_back(acl_);
      }

      foreach (const ACL::LaunchNestedContainerUnderParentWithUser& acl,
               acls.launch_nested_containers_under_parent_with_user()) {
        GenericACL acl_;
        acl_.subjects = acl.principals();
        acl_.objects = acl.users();

        parentRunningAsUserAcls.push_back(acl_);
      }
    } else {
      foreach (const ACL::LaunchNestedContainerSessionAsUser& acl,
               acls.launch_nested_container_sessions_as_user()) {
        GenericACL acl_;
        acl_.subjects = acl.principals();
        acl_.objects = acl.users();

        runAsUserAcls.push_back(acl_);
      }

      foreach (const ACL::LaunchNestedContainerSessionUnderParentWithUser& acl,
               acls.launch_nested_container_sessions_under_parent_with_user())
      {
        GenericACL acl_;
        acl_.subjects = acl.principals();
        acl_.objects = acl.users();

        parentRunningAsUserAcls.push_back(acl_);
      }
    }

    return std::make_shared<LocalNestedContainerObjectApprover>(
        runAsUserAcls,
        parentRunningAsUserAcls,
        subject,
        action,
        acls.permissive());
  }

  shared_ptr<const ObjectApprover> getImplicitExecutorObjectApprover(
      const Option<authorization::Subject>& subject,
      const authorization::Action& action)
  {
    CHECK(subject.isSome() &&
          subject->has_claims() &&
          !subject->has_value() &&
          (action == authorization::LAUNCH_NESTED_CONTAINER ||
           action == authorization::WAIT_NESTED_CONTAINER ||
           action == authorization::KILL_NESTED_CONTAINER ||
           action == authorization::LAUNCH_NESTED_CONTAINER_SESSION ||
           action == authorization::REMOVE_NESTED_CONTAINER ||
           action == authorization::ATTACH_CONTAINER_OUTPUT));

    Option<ContainerID> subjectContainerId;
    foreach (const Label& claim, subject->claims().labels()) {
      if (claim.key() == "cid" && claim.has_value()) {
        subjectContainerId = ContainerID();
        subjectContainerId->set_value(claim.value());
        break;
      }
    }

    if (subjectContainerId.isNone()) {
      // If the subject's claims do not include a ContainerID,
      // we deny all objects.
      return std::make_shared<RejectingObjectApprover>();
    }

    return std::make_shared<LocalImplicitExecutorObjectApprover>(
        subjectContainerId.get());
  }

  shared_ptr<const ObjectApprover> getImplicitResourceProviderObjectApprover(
      const Option<authorization::Subject>& subject,
      const authorization::Action& action)
  {
    CHECK(subject.isSome() &&
          subject->has_claims() &&
          !subject->has_value() &&
          (action == authorization::LAUNCH_STANDALONE_CONTAINER ||
           action == authorization::WAIT_STANDALONE_CONTAINER ||
           action == authorization::KILL_STANDALONE_CONTAINER ||
           action == authorization::REMOVE_STANDALONE_CONTAINER ||
           action == authorization::VIEW_STANDALONE_CONTAINER));

    Option<string> subjectPrefix;
    foreach (const Label& claim, subject->claims().labels()) {
      if (claim.key() == "cid_prefix" && claim.has_value()) {
        subjectPrefix = claim.value();
      }
    }

    if (subjectPrefix.isNone()) {
      // If the subject's claims do not include a namespace string,
      // we deny all objects.
      return std::make_shared<RejectingObjectApprover>();
    }

    return std::make_shared<LocalImplicitResourceProviderObjectApprover>(
        subjectPrefix.get());
  }

  Try<shared_ptr<const ObjectApprover>> getApprover(
      const Option<authorization::Subject>& subject,
      const authorization::Action& action)
  {
    // We return implicit object approvers only for subjects and actions
    // which comes from either the default executor or a local resource
    // provider. This means the subject should have claims but no value,
    // and the action should be one of the actions used by them.
    if (subject.isSome() &&
        subject->has_claims() &&
        !subject->has_value()) {
      // The `LocalImplicitExecutorObjectApprover` is used to authorize
      // requests from the default executor.
      if (action == authorization::LAUNCH_NESTED_CONTAINER ||
          action == authorization::WAIT_NESTED_CONTAINER ||
          action == authorization::KILL_NESTED_CONTAINER ||
          action == authorization::LAUNCH_NESTED_CONTAINER_SESSION ||
          action == authorization::REMOVE_NESTED_CONTAINER ||
          action == authorization::ATTACH_CONTAINER_OUTPUT) {
        return getImplicitExecutorObjectApprover(subject, action);
      }

      // The `LocalImplicitResourceProviderObjectApprover` is used to
      // authorize requests from a local resource provider.
      if (action == authorization::LAUNCH_STANDALONE_CONTAINER ||
          action == authorization::WAIT_STANDALONE_CONTAINER ||
          action == authorization::KILL_STANDALONE_CONTAINER ||
          action == authorization::REMOVE_STANDALONE_CONTAINER ||
          action == authorization::VIEW_STANDALONE_CONTAINER) {
        return getImplicitResourceProviderObjectApprover(subject, action);
      }
    }

    // Currently, implicit executor authorization is the only case which handles
    // subjects that do not have the `value` field set. If the previous case was
    // not true and `value` is not set, then we should fail all requests.
    if (subject.isSome() && !subject->has_value()) {
      return std::make_shared<RejectingObjectApprover>();
    }

    switch (action) {
      case authorization::LAUNCH_NESTED_CONTAINER:
      case authorization::LAUNCH_NESTED_CONTAINER_SESSION: {
        return getNestedContainerObjectApprover(subject, action);
      }
      case authorization::CREATE_VOLUME:
      case authorization::RESIZE_VOLUME:
      case authorization::RESERVE_RESOURCES:
      case authorization::UPDATE_WEIGHT:
      case authorization::VIEW_ROLE:
      case authorization::GET_QUOTA:
      case authorization::REGISTER_FRAMEWORK:
      case authorization::UPDATE_QUOTA:
      case authorization::UPDATE_QUOTA_WITH_CONFIG:
      case authorization::CREATE_BLOCK_DISK:
      case authorization::DESTROY_BLOCK_DISK:
      case authorization::CREATE_MOUNT_DISK:
      case authorization::DESTROY_MOUNT_DISK:
      case authorization::DESTROY_RAW_DISK: {
        return getHierarchicalRoleApprover(subject, action);
      }
      case authorization::ACCESS_MESOS_LOG:
      case authorization::ACCESS_SANDBOX:
      case authorization::ATTACH_CONTAINER_INPUT:
      case authorization::ATTACH_CONTAINER_OUTPUT:
      case authorization::DESTROY_VOLUME:
      case authorization::GET_ENDPOINT_WITH_PATH:
      case authorization::GET_MAINTENANCE_SCHEDULE:
      case authorization::GET_MAINTENANCE_STATUS:
      case authorization::KILL_NESTED_CONTAINER:
      case authorization::KILL_STANDALONE_CONTAINER:
      case authorization::LAUNCH_STANDALONE_CONTAINER:
      case authorization::MARK_AGENT_GONE:
      case authorization::PRUNE_IMAGES:
      case authorization::REGISTER_AGENT:
      case authorization::REMOVE_NESTED_CONTAINER:
      case authorization::REMOVE_STANDALONE_CONTAINER:
      case authorization::RUN_TASK:
      case authorization::SET_LOG_LEVEL:
      case authorization::START_MAINTENANCE:
      case authorization::STOP_MAINTENANCE:
      case authorization::TEARDOWN_FRAMEWORK:
      case authorization::UNRESERVE_RESOURCES:
      case authorization::UPDATE_MAINTENANCE_SCHEDULE:
      case authorization::DRAIN_AGENT:
      case authorization::DEACTIVATE_AGENT:
      case authorization::REACTIVATE_AGENT:
      case authorization::VIEW_CONTAINER:
      case authorization::VIEW_EXECUTOR:
      case authorization::VIEW_FLAGS:
      case authorization::VIEW_FRAMEWORK:
      case authorization::VIEW_STANDALONE_CONTAINER:
      case authorization::VIEW_TASK:
      case authorization::WAIT_NESTED_CONTAINER:
      case authorization::WAIT_STANDALONE_CONTAINER:
      case authorization::MODIFY_RESOURCE_PROVIDER_CONFIG:
      case authorization::MARK_RESOURCE_PROVIDER_GONE:
      case authorization::VIEW_RESOURCE_PROVIDER:
      case authorization::UNKNOWN: {
        Result<vector<GenericACL>> genericACLs =
          createGenericACLs(action, acls);
        if (genericACLs.isError()) {
          return Error(genericACLs.error());
        }
        if (genericACLs.isNone()) {
          // If we could not create acls, we deny all objects.
          return std::make_shared<RejectingObjectApprover>();
        }

        return std::make_shared<LocalAuthorizerObjectApprover>(
            genericACLs.get(), subject, action, acls.permissive());
      }
    }

    UNREACHABLE();
  }

private:
  static Result<vector<GenericACL>> createGenericACLs(
      const authorization::Action& action,
      const ACLs& acls)
  {
    vector<GenericACL> acls_;

    switch (action) {
      case authorization::TEARDOWN_FRAMEWORK:
        foreach (
            const ACL::TeardownFramework& acl, acls.teardown_frameworks()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.framework_principals();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::RUN_TASK:
        foreach (const ACL::RunTask& acl, acls.run_tasks()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::UNRESERVE_RESOURCES:
        foreach (
            const ACL::UnreserveResources& acl, acls.unreserve_resources()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.reserver_principals();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::DESTROY_VOLUME:
        foreach (const ACL::DestroyVolume& acl, acls.destroy_volumes()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.creator_principals();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::GET_ENDPOINT_WITH_PATH:
        foreach (const ACL::GetEndpoint& acl, acls.get_endpoints()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.paths();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::ACCESS_MESOS_LOG:
        foreach (const ACL::AccessMesosLog& acl, acls.access_mesos_logs()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.logs();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::VIEW_FLAGS:
        foreach (const ACL::ViewFlags& acl, acls.view_flags()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.flags();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::ACCESS_SANDBOX:
        foreach (const ACL::AccessSandbox& acl, acls.access_sandboxes()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::ATTACH_CONTAINER_INPUT:
        foreach (const ACL::AttachContainerInput& acl,
            acls.attach_containers_input()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::ATTACH_CONTAINER_OUTPUT:
        foreach (const ACL::AttachContainerOutput& acl,
            acls.attach_containers_output()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::KILL_NESTED_CONTAINER:
        foreach (const ACL::KillNestedContainer& acl,
            acls.kill_nested_containers()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::WAIT_NESTED_CONTAINER:
        foreach (const ACL::WaitNestedContainer& acl,
            acls.wait_nested_containers()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::REMOVE_NESTED_CONTAINER:
        foreach (const ACL::RemoveNestedContainer& acl,
            acls.remove_nested_containers()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::VIEW_FRAMEWORK:
        foreach (const ACL::ViewFramework& acl, acls.view_frameworks()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::VIEW_TASK:
        foreach (const ACL::ViewTask& acl, acls.view_tasks()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::VIEW_EXECUTOR:
        foreach (const ACL::ViewExecutor& acl, acls.view_executors()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::SET_LOG_LEVEL:
        foreach (const ACL::SetLogLevel& acl, acls.set_log_level()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.level();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::VIEW_CONTAINER:
        foreach (const ACL::ViewContainer& acl, acls.view_containers()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::REGISTER_AGENT:
        foreach (const ACL::RegisterAgent& acl, acls.register_agents()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.agents();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::UPDATE_MAINTENANCE_SCHEDULE:
        foreach (const ACL::UpdateMaintenanceSchedule& acl,
                 acls.update_maintenance_schedules()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.machines();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::GET_MAINTENANCE_SCHEDULE:
        foreach (const ACL::GetMaintenanceSchedule& acl,
                 acls.get_maintenance_schedules()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.machines();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::START_MAINTENANCE:
        foreach (const ACL::StartMaintenance& acl, acls.start_maintenances()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.machines();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::STOP_MAINTENANCE:
        foreach (const ACL::StopMaintenance& acl, acls.stop_maintenances()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.machines();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::GET_MAINTENANCE_STATUS:
        foreach (const ACL::GetMaintenanceStatus& acl,
                 acls.get_maintenance_statuses()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.machines();

          acls_.push_back(acl_);
        }

        return acls_;

      case authorization::DRAIN_AGENT:
        foreach (const ACL::DrainAgent& acl,
                 acls.drain_agents()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.agents();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::DEACTIVATE_AGENT:
        foreach (const ACL::DeactivateAgent& acl,
                 acls.deactivate_agents()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.agents();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::REACTIVATE_AGENT:
        foreach (const ACL::ReactivateAgent& acl,
                 acls.reactivate_agents()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.agents();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::MARK_AGENT_GONE:
        foreach (const ACL::MarkAgentGone& acl,
                 acls.mark_agents_gone()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.agents();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::LAUNCH_STANDALONE_CONTAINER:
        foreach (const ACL::LaunchStandaloneContainer& acl,
                 acls.launch_standalone_containers()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::KILL_STANDALONE_CONTAINER:
        foreach (const ACL::KillStandaloneContainer& acl,
            acls.kill_standalone_containers()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::WAIT_STANDALONE_CONTAINER:
        foreach (const ACL::WaitStandaloneContainer& acl,
            acls.wait_standalone_containers()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::REMOVE_STANDALONE_CONTAINER:
        foreach (const ACL::RemoveStandaloneContainer& acl,
            acls.remove_standalone_containers()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::VIEW_STANDALONE_CONTAINER:
        foreach (const ACL::ViewStandaloneContainer& acl,
            acls.view_standalone_containers()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::MODIFY_RESOURCE_PROVIDER_CONFIG:
        foreach (const ACL::ModifyResourceProviderConfig& acl,
                 acls.modify_resource_provider_configs()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.resource_providers();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::MARK_RESOURCE_PROVIDER_GONE:
        foreach (const ACL::MarkResourceProvidersGone& acl,
                 acls.mark_resource_providers_gone()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.resource_providers();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::VIEW_RESOURCE_PROVIDER:
        foreach (
            const ACL::ViewResourceProvider& acl,
            acls.view_resource_providers()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.resource_providers();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::PRUNE_IMAGES:
        foreach (const ACL::PruneImages& acl, acls.prune_images()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.images();

          acls_.push_back(acl_);
        }

        return acls_;
      case authorization::REGISTER_FRAMEWORK:
      case authorization::CREATE_VOLUME:
      case authorization::RESIZE_VOLUME:
      case authorization::RESERVE_RESOURCES:
      case authorization::UPDATE_WEIGHT:
      case authorization::VIEW_ROLE:
      case authorization::GET_QUOTA:
      case authorization::UPDATE_QUOTA:
      case authorization::UPDATE_QUOTA_WITH_CONFIG:
      case authorization::LAUNCH_NESTED_CONTAINER_SESSION:
      case authorization::LAUNCH_NESTED_CONTAINER:
      case authorization::CREATE_BLOCK_DISK:
      case authorization::DESTROY_BLOCK_DISK:
      case authorization::CREATE_MOUNT_DISK:
      case authorization::DESTROY_MOUNT_DISK:
      case authorization::DESTROY_RAW_DISK:
        return Error("Extracting ACLs for " + stringify(action) + " requires "
                     "a specialized function");
      case authorization::UNKNOWN:
        // Cannot generate acls for an unknown action.
        return None();
    }
    UNREACHABLE();
  }

  ACLs acls;
};


Try<Authorizer*> LocalAuthorizer::create(const ACLs& acls)
{
  Option<Error> validationError = validate(acls);
  if (validationError.isSome()) {
    return validationError.get();
  }

  Authorizer* local = new LocalAuthorizer(acls);

  return local;
}


Try<Authorizer*> LocalAuthorizer::create(const Parameters& parameters)
{
  Option<string> acls;
  foreach (const Parameter& parameter, parameters.parameter()) {
    if (parameter.key() == "acls") {
      acls = parameter.value();
    }
  }

  if (acls.isNone()) {
    return Error("No ACLs for default authorizer provided");
  }

  Try<ACLs> acls_ = flags::parse<ACLs>(acls.get());
  if (acls_.isError()) {
    return Error("Contents of 'acls' parameter could not be parsed into a "
                 "valid ACLs object");
  }

  return LocalAuthorizer::create(acls_.get());
}


Option<Error> LocalAuthorizer::validate(const ACLs& acls)
{
  foreach (const ACL::AccessMesosLog& acl, acls.access_mesos_logs()) {
    if (acl.logs().type() == ACL::Entity::SOME) {
      return Error("ACL.AccessMesosLog type must be either NONE or ANY");
    }
  }

  foreach (const ACL::ViewFlags& acl, acls.view_flags()) {
    if (acl.flags().type() == ACL::Entity::SOME) {
      return Error("ACL.ViewFlags type must be either NONE or ANY");
    }
  }

  foreach (const ACL::SetLogLevel& acl, acls.set_log_level()) {
    if (acl.level().type() == ACL::Entity::SOME) {
      return Error("ACL.SetLogLevel type must be either NONE or ANY");
    }
  }

  foreach (const ACL::GetEndpoint& acl, acls.get_endpoints()) {
    if (acl.paths().type() == ACL::Entity::SOME) {
      foreach (const string& path, acl.paths().values()) {
        if (!authorization::AUTHORIZABLE_ENDPOINTS.contains(path)) {
          return Error("Path: '" + path + "' is not an authorizable path");
        }
      }
    }
  }

  foreach (const ACL::RegisterAgent& acl, acls.register_agents()) {
    if (acl.agents().type() == ACL::Entity::SOME) {
      return Error(
          "ACL.RegisterAgent type must be either NONE or ANY");
    }
  }

  foreach (const ACL::UpdateMaintenanceSchedule& acl,
           acls.update_maintenance_schedules()) {
    if (acl.machines().type() == ACL::Entity::SOME) {
      return Error(
          "ACL.UpdateMaintenanceSchedule type must be either NONE or ANY");
    }
  }

  foreach (const ACL::GetMaintenanceSchedule& acl,
           acls.get_maintenance_schedules()) {
    if (acl.machines().type() == ACL::Entity::SOME) {
      return Error(
          "ACL.GetMaintenanceSchedule type must be either NONE or ANY");
    }
  }

  foreach (const ACL::StartMaintenance& acl, acls.start_maintenances()) {
    if (acl.machines().type() == ACL::Entity::SOME) {
      return Error("ACL.StartMaintenance type must be either NONE or ANY");
    }
  }

  foreach (const ACL::StopMaintenance& acl, acls.stop_maintenances()) {
    if (acl.machines().type() == ACL::Entity::SOME) {
      return Error("ACL.StopMaintenance type must be either NONE or ANY");
    }
  }

  foreach (const ACL::GetMaintenanceStatus& acl,
           acls.get_maintenance_statuses()) {
    if (acl.machines().type() == ACL::Entity::SOME) {
      return Error("ACL.GetMaintenanceStatus type must be either NONE or ANY");
    }
  }

  foreach (const ACL::DrainAgent& acl, acls.drain_agents()) {
    if (acl.agents().type() == ACL::Entity::SOME) {
      return Error("ACL.DrainAgent type must be either NONE or ANY");
    }
  }

  foreach (const ACL::DeactivateAgent& acl, acls.deactivate_agents()) {
    if (acl.agents().type() == ACL::Entity::SOME) {
      return Error("ACL.DeactivateAgent type must be either NONE or ANY");
    }
  }

  foreach (const ACL::ReactivateAgent& acl, acls.reactivate_agents()) {
    if (acl.agents().type() == ACL::Entity::SOME) {
      return Error("ACL.ReactivateAgent type must be either NONE or ANY");
    }
  }

  foreach (const ACL::LaunchStandaloneContainer& acl,
           acls.launch_standalone_containers()) {
    if (acl.users().type() == ACL::Entity::SOME) {
      return Error(
          "ACL.LaunchStandaloneContainer type must be either NONE or ANY");
    }
  }

  foreach (const ACL::KillStandaloneContainer& acl,
           acls.kill_standalone_containers()) {
    if (acl.users().type() == ACL::Entity::SOME) {
      return Error(
          "ACL.KillStandaloneContainer type must be either NONE or ANY");
    }
  }

  foreach (const ACL::WaitStandaloneContainer& acl,
           acls.wait_standalone_containers()) {
    if (acl.users().type() == ACL::Entity::SOME) {
      return Error(
          "ACL.WaitStandaloneContainer type must be either NONE or ANY");
    }
  }

  foreach (const ACL::RemoveStandaloneContainer& acl,
           acls.remove_standalone_containers()) {
    if (acl.users().type() == ACL::Entity::SOME) {
      return Error(
          "ACL.RemoveStandaloneContainer type must be either NONE or ANY");
    }
  }

  foreach (const ACL::ViewStandaloneContainer& acl,
           acls.view_standalone_containers()) {
    if (acl.users().type() == ACL::Entity::SOME) {
      return Error(
          "ACL.ViewStandaloneContainer type must be either NONE or ANY");
    }
  }

  foreach (const ACL::MarkResourceProvidersGone& acl,
           acls.mark_resource_providers_gone()) {
    if (acl.resource_providers().type() == ACL::Entity::SOME) {
      return Error(
          "ACL.MarkResourceProvidersGone type must be either NONE or ANY");
    }
  }

  foreach (const ACL::ViewResourceProvider& acl,
           acls.view_resource_providers()) {
    if (acl.resource_providers().type() == ACL::Entity::SOME) {
      return Error("ACL.ViewResourceProvider type must be either NONE or ANY");
    }
  }

  foreach (const ACL::ModifyResourceProviderConfig& acl,
           acls.modify_resource_provider_configs()) {
    if (acl.resource_providers().type() == ACL::Entity::SOME) {
      return Error(
          "ACL.ModifyResourceProviderConfig type must be either NONE or ANY");
    }
  }

  foreach (const ACL::PruneImages& acl, acls.prune_images()) {
    if (acl.images().type() == ACL::Entity::SOME) {
      return Error("ACL.PruneImages type must be either NONE or ANY");
    }
  }

  // TODO(alexr): Consider validating not only protobuf, but also the original
  // JSON in order to spot misspelled names. A misspelled action may affect
  // authorization result and hence lead to a security issue (e.g. when there
  // are entries with same action but different subjects or objects).

  return None();
}


LocalAuthorizer::LocalAuthorizer(const ACLs& acls)
    : process(new LocalAuthorizerProcess(acls))
{
  spawn(process);
}


LocalAuthorizer::~LocalAuthorizer()
{
  if (process != nullptr) {
    terminate(process);
    wait(process);
    delete process;
  }
}


process::Future<bool> LocalAuthorizer::authorized(
  const authorization::Request& request)
{
  // Request sanity checks.
  // A set `subject` should always come with a set `value` or `claims`.
  CHECK(
    !request.has_subject() ||
    request.subject().has_value() ||
    request.subject().has_claims());

  // A set `action` is mandatory.
  CHECK(request.has_action());

  // A set `object` should always come with at least one set union
  // style value.
  CHECK(
    !request.has_object() ||
    (request.has_object() &&
     (request.object().has_value() ||
      request.object().has_framework_info() ||
      request.object().has_task() ||
      request.object().has_task_info() ||
      request.object().has_executor_info() ||
      request.object().has_quota_info() ||
      request.object().has_weight_info() ||
      request.object().has_container_id() ||
      request.object().has_resource())));

  typedef Future<bool> (LocalAuthorizerProcess::*F)(
      const authorization::Request&);

  return dispatch(
      process,
      static_cast<F>(&LocalAuthorizerProcess::authorized),
      request);
}


Future<shared_ptr<const ObjectApprover>> LocalAuthorizer::getApprover(
    const Option<authorization::Subject>& subject,
    const authorization::Action& action)
{
  return dispatch(
      process,
      &LocalAuthorizerProcess::getApprover,
      subject,
      action)
    .then(
        [](const Try<shared_ptr<const ObjectApprover>>& approver)
          -> Future<shared_ptr<const ObjectApprover>> {
          if (approver.isError()) {
            return Failure(approver.error());
          }

          return *approver;
        });
}

} // namespace internal {
} // namespace mesos {
