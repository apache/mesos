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
#include <stout/try.hpp>

#include "common/parse.hpp"

using process::dispatch;
using process::Failure;
using process::Future;
using process::Owned;

using std::string;
using std::vector;

namespace mesos {
namespace internal {

struct GenericACL
{
  ACL::Entity subjects;
  ACL::Entity objects;
};


// TODO(mpark): This class exists to optionally carry `ACL::SetQuota` and
// `ACL::RemoveQuota` ACLs. This is a hack to support the deprecation cycle for
// `ACL::SetQuota` and `ACL::RemoveQuota`. This can be removed / replaced with
// `vector<GenericACL>` at the end of deprecation cycle which started with 1.0.
struct GenericACLs
{
  GenericACLs(const vector<GenericACL>& acls_) : acls(acls_) {}

  GenericACLs(
      const vector<GenericACL>& acls_,
      const vector<GenericACL>& set_quotas_,
      const vector<GenericACL>& remove_quotas_)
    : acls(acls_), set_quotas(set_quotas_), remove_quotas(remove_quotas_) {}

  vector<GenericACL> acls;

  // These ACLs are set iff the authorization action is `UPDATE_QUOTA`.
  Option<vector<GenericACL>> set_quotas;
  Option<vector<GenericACL>> remove_quotas;
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
      const GenericACLs& acls,
      const Option<authorization::Subject>& subject,
      const authorization::Action& action,
      const bool permissive)
    : acls_(acls),
      subject_(subject),
      action_(action),
      permissive_(permissive) {}

  virtual Try<bool> approved(
      const Option<ObjectApprover::Object>& object) const noexcept override {
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
        // All actions using `object.value` for authorization.
        case authorization::REGISTER_FRAMEWORK_WITH_ROLE:
        case authorization::TEARDOWN_FRAMEWORK_WITH_PRINCIPAL:
        case authorization::RESERVE_RESOURCES_WITH_ROLE:
        case authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL:
        case authorization::CREATE_VOLUME_WITH_ROLE:
        case authorization::DESTROY_VOLUME_WITH_PRINCIPAL:
        case authorization::GET_QUOTA_WITH_ROLE:
        case authorization::GET_WEIGHT_WITH_ROLE:
        case authorization::UPDATE_WEIGHT_WITH_ROLE:
        case authorization::GET_ENDPOINT_WITH_PATH: {
          // Check object has the required types set.
          CHECK_NOTNULL(object->value);

          aclObject.add_values(*(object->value));
          aclObject.set_type(mesos::ACL::Entity::SOME);

          break;
        }
        case authorization::RUN_TASK: {
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
        }
        case authorization::ACCESS_MESOS_LOG: {
          aclObject.set_type(mesos::ACL::Entity::ANY);

          break;
        }
        case authorization::ACCESS_SANDBOX: {
          aclObject.set_type(mesos::ACL::Entity::ANY);

          if (object->executor_info != NULL &&
              object->executor_info->command().has_user()) {
            aclObject.add_values(object->executor_info->command().user());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          } else if (object->framework_info != NULL) {
            aclObject.add_values(object->framework_info->user());
            aclObject.set_type(mesos::ACL::Entity::SOME);
          }

          break;
        }
        case authorization::UPDATE_QUOTA: {
          // Check object has the required types set.
          CHECK_NOTNULL(object->quota_info);

          // TODO(mpark): This is a hack to support the deprecation cycle for
          // `ACL::SetQuota` and `ACL::RemoveQuota`. This block of code can be
          // removed at the end of deprecation cycle which started with 1.0.
          if (acls_.set_quotas->size() > 0 || acls_.remove_quotas->size() > 0) {
            CHECK_NOTNULL(object->value);
            if (*object->value == "SetQuota") {
              aclObject.add_values(object->quota_info->role());
              aclObject.set_type(mesos::ACL::Entity::SOME);

              CHECK_SOME(acls_.set_quotas);
              return approved(acls_.set_quotas.get(), aclSubject, aclObject);
            } else if (*object->value == "RemoveQuota") {
              if (object->quota_info->has_principal()) {
                aclObject.add_values(object->quota_info->principal());
                aclObject.set_type(mesos::ACL::Entity::SOME);
              } else {
                aclObject.set_type(mesos::ACL::Entity::ANY);
              }

              CHECK_SOME(acls_.remove_quotas);
              return approved(acls_.remove_quotas.get(), aclSubject, aclObject);
            }
          }

          aclObject.add_values(object->quota_info->role());
          aclObject.set_type(mesos::ACL::Entity::SOME);

          break;
        }
        case authorization::VIEW_FRAMEWORK: {
          // Check object has the required types set.
          CHECK_NOTNULL(object->framework_info);

          aclObject.add_values(object->framework_info->user());
          aclObject.set_type(mesos::ACL::Entity::SOME);

          break;
        }
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
        case authorization::VIEW_EXECUTOR: {
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
        }
        case authorization::UNKNOWN:
          LOG(WARNING) << "Authorization for action '" << action_
                       << "' is not defined and therefore not authorized";
          return false;
          break;
      }
    }

    return approved(acls_.acls, aclSubject, aclObject);
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

  const GenericACLs acls_;
  const Option<authorization::Subject> subject_;
  const authorization::Action action_;
  const bool permissive_;
};


class LocalAuthorizerProcess : public ProtobufProcess<LocalAuthorizerProcess>
{
public:
  LocalAuthorizerProcess(const ACLs& _acls)
    : ProcessBase(process::ID::generate("authorizer")), acls(_acls) {}

  virtual void initialize()
  {
    // TODO(zhitao): Remove the following log warning at the end of the
    // deprecation cycle which started with 1.0.
    if (acls.set_quotas_size() > 0 ||
        acls.remove_quotas_size() > 0) {
      LOG(WARNING) << "SetQuota and RemoveQuota ACLs are deprecated; "
                   << "please use UpdateQuota";
    }

    // TODO(arojas): Remove the following two if blocks once
    // ShutdownFramework reaches the end of deprecation cycle
    // which started with 0.27.0.
    if (acls.shutdown_frameworks_size() > 0 &&
        acls.teardown_frameworks_size() > 0) {
      LOG(WARNING) << "ACLs defined for both ShutdownFramework and "
                   << "TeardownFramework; only the latter will be used";
      return;
    }

    // Move contents of `acls.shutdown_frameworks` to
    // `acls.teardown_frameworks`
    if (acls.shutdown_frameworks_size() > 0) {
      LOG(WARNING) << "ShutdownFramework ACL is deprecated; please use "
                   << "TeardownFramework";
      foreach (const ACL::ShutdownFramework& acl, acls.shutdown_frameworks()) {
        ACL::TeardownFramework* teardown = acls.add_teardown_frameworks();
        teardown->mutable_principals()->CopyFrom(acl.principals());
        teardown->mutable_framework_principals()->CopyFrom(
            acl.framework_principals());
      }
    }
    acls.clear_shutdown_frameworks();
  }

  Future<bool> authorized(const authorization::Request& request)
  {
    return getObjectApprover(request.subject(), request.action())
      .then([=](const Owned<ObjectApprover>& objectApprover) -> Future<bool> {
        Option<ObjectApprover::Object> object = None();
        if (request.has_object()) {
          object = ObjectApprover::Object(request.object());
        }

        Try<bool> result = objectApprover->approved(object);
        if (result.isError()) {
          return Failure(result.error());
        }
        return result.get();
      });
  }

  Future<Owned<ObjectApprover>> getObjectApprover(
      const Option<authorization::Subject>& subject,
      const authorization::Action& action)
  {
    // Implementation of the ObjectApprover interface denying all objects.
    class RejectingObjectApprover : public ObjectApprover
    {
    public:
      virtual Try<bool> approved(
          const Option<ObjectApprover::Object>& object) const noexcept override
      {
        return false;
      }
    };

    // Generate GenericACLs.
    Result<GenericACLs> genericACLs = createGenericACLs(action, acls);
    if (genericACLs.isError()) {
      return Failure(genericACLs.error());
    }

    if (genericACLs.isNone()) {
      // If we could not create acls, we deny all objects.
      return Owned<ObjectApprover>(new RejectingObjectApprover());
    }

    return Owned<ObjectApprover>(
        new LocalAuthorizerObjectApprover(
            genericACLs.get(), subject, action, acls.permissive()));
  }

private:
  static Result<GenericACLs> createGenericACLs(
      const authorization::Action& action,
      const ACLs& acls)
  {
    vector<GenericACL> acls_;

    switch (action) {
      case authorization::REGISTER_FRAMEWORK_WITH_ROLE:
        foreach (
            const ACL::RegisterFramework& acl, acls.register_frameworks()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::TEARDOWN_FRAMEWORK_WITH_PRINCIPAL:
        foreach (
            const ACL::TeardownFramework& acl, acls.teardown_frameworks()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.framework_principals();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::RUN_TASK:
        foreach (const ACL::RunTask& acl, acls.run_tasks()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::RESERVE_RESOURCES_WITH_ROLE:
        foreach (const ACL::ReserveResources& acl, acls.reserve_resources()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL:
        foreach (
            const ACL::UnreserveResources& acl, acls.unreserve_resources()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.reserver_principals();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::CREATE_VOLUME_WITH_ROLE:
        foreach (const ACL::CreateVolume& acl, acls.create_volumes()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::DESTROY_VOLUME_WITH_PRINCIPAL:
        foreach (const ACL::DestroyVolume& acl, acls.destroy_volumes()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.creator_principals();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::GET_QUOTA_WITH_ROLE:
        foreach (const ACL::GetQuota& acl, acls.get_quotas()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::UPDATE_QUOTA: {
        foreach (const ACL::UpdateQuota& acl, acls.update_quotas()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          acls_.push_back(acl_);
        }

        vector<GenericACL> set_quotas;
        foreach (const ACL::SetQuota& acl, acls.set_quotas()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          set_quotas.push_back(acl_);
        }

        vector<GenericACL> remove_quotas;
        foreach (const ACL::RemoveQuota& acl, acls.remove_quotas()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.quota_principals();

          remove_quotas.push_back(acl_);
        }

        return GenericACLs(acls_, set_quotas, remove_quotas);
        break;
      }
      case authorization::GET_WEIGHT_WITH_ROLE:
        foreach (const ACL::GetWeight& acl, acls.get_weights()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::UPDATE_WEIGHT_WITH_ROLE:
        foreach (const ACL::UpdateWeight& acl, acls.update_weights()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::GET_ENDPOINT_WITH_PATH:
        foreach (const ACL::GetEndpoint& acl, acls.get_endpoints()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.paths();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::ACCESS_MESOS_LOG:
        foreach (const ACL::AccessMesosLog& acl, acls.access_mesos_logs()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.logs();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::ACCESS_SANDBOX: {
        foreach (const ACL::AccessSandbox& acl, acls.access_sandboxes()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      }
      case authorization::VIEW_FRAMEWORK:
        foreach (const ACL::ViewFramework& acl, acls.view_frameworks()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::VIEW_TASK:
        foreach (const ACL::ViewTask& acl, acls.view_tasks()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::VIEW_EXECUTOR:
        foreach (const ACL::ViewExecutor& acl, acls.view_executors()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return acls_;
        break;
      case authorization::UNKNOWN:
        // Cannot generate acls for an unknown action.
        return None();
        break;
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
  if (acls.update_quotas_size() > 0 &&
      (acls.set_quotas_size() > 0 || acls.remove_quotas_size() > 0)) {
    return Error("acls.update_quotas cannot be used "
                 "together with deprecated set_quotas/remove_quotas!");
  }


  foreach (const ACL::AccessMesosLog& acl, acls.access_mesos_logs()) {
    if (acl.logs().type() == ACL::Entity::SOME) {
      return Error("acls.access_mesos_logs type must be either NONE or ANY");
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
  // A set `subject` should always come with a set `value`.
  CHECK(
    !request.has_subject() ||
    (request.has_subject() && request.subject().has_value()));

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
      request.object().has_quota_info())));

  typedef Future<bool> (LocalAuthorizerProcess::*F)(
      const authorization::Request&);

  return dispatch(
      process,
      static_cast<F>(&LocalAuthorizerProcess::authorized),
      request);
}


Future<Owned<ObjectApprover>> LocalAuthorizer::getObjectApprover(
      const Option<authorization::Subject>& subject,
      const authorization::Action& action)
{
  return dispatch(
      process,
      &LocalAuthorizerProcess::getObjectApprover,
      subject,
      action);
}

} // namespace internal {
} // namespace mesos {
