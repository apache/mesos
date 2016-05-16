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
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include "common/parse.hpp"

using process::dispatch;
using process::Failure;
using process::Future;

using std::string;
using std::vector;

namespace mesos {
namespace internal {

struct GenericACL
{
  ACL::Entity subjects;
  ACL::Entity objects;
};


class LocalAuthorizerProcess : public ProtobufProcess<LocalAuthorizerProcess>
{
public:
  LocalAuthorizerProcess(const ACLs& _acls)
    : ProcessBase(process::ID::generate("authorizer")), acls(_acls) {}

  virtual void initialize()
  {
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
    vector<GenericACL> acls_;

    switch (request.action()) {
      case authorization::REGISTER_FRAMEWORK_WITH_ROLE:
        foreach (
            const ACL::RegisterFramework& acl, acls.register_frameworks()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          acls_.push_back(acl_);
        }

        return authorized(request, acls_);
        break;
      case authorization::TEARDOWN_FRAMEWORK_WITH_PRINCIPAL:
        foreach (
            const ACL::TeardownFramework& acl, acls.teardown_frameworks()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.framework_principals();

          acls_.push_back(acl_);
        }

        return authorized(request, acls_);
        break;
      case authorization::RUN_TASK_WITH_USER:
        foreach (const ACL::RunTask& acl, acls.run_tasks()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.users();

          acls_.push_back(acl_);
        }

        return authorized(request, acls_);
        break;
      case authorization::RESERVE_RESOURCES_WITH_ROLE:
        foreach (const ACL::ReserveResources& acl, acls.reserve_resources()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          acls_.push_back(acl_);
        }

        return authorized(request, acls_);
        break;
      case authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL:
        foreach (
            const ACL::UnreserveResources& acl, acls.unreserve_resources()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.reserver_principals();

          acls_.push_back(acl_);
        }

        return authorized(request, acls_);
        break;
      case authorization::CREATE_VOLUME_WITH_ROLE:
        foreach (const ACL::CreateVolume& acl, acls.create_volumes()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          acls_.push_back(acl_);
        }

        return authorized(request, acls_);
        break;
      case authorization::DESTROY_VOLUME_WITH_PRINCIPAL:
        foreach (const ACL::DestroyVolume& acl, acls.destroy_volumes()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.creator_principals();

          acls_.push_back(acl_);
        }

        return authorized(request, acls_);
        break;
      case authorization::SET_QUOTA_WITH_ROLE:
        foreach (const ACL::SetQuota& acl, acls.set_quotas()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          acls_.push_back(acl_);
        }

        return authorized(request, acls_);
        break;
      case authorization::DESTROY_QUOTA_WITH_PRINCIPAL:
        foreach (const ACL::RemoveQuota& acl, acls.remove_quotas()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.quota_principals();

          acls_.push_back(acl_);
        }

        return authorized(request, acls_);
        break;
      case authorization::UPDATE_WEIGHTS_WITH_ROLE:
        foreach (const ACL::UpdateWeights& acl, acls.update_weights()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.roles();

          acls_.push_back(acl_);
        }

        return authorized(request, acls_);
        break;
      case authorization::GET_ENDPOINT_WITH_PATH:
        foreach (const ACL::GetEndpoint& acl, acls.get_endpoints()) {
          GenericACL acl_;
          acl_.subjects = acl.principals();
          acl_.objects = acl.paths();

          acls_.push_back(acl_);
        }

        return authorized(request, acls_);
        break;
      case authorization::UNKNOWN:
        LOG(WARNING) << "Authorization request for action '" << request.action()
                     << "' is not defined and therefore not authorized";
        return false;
        break;
    }
    UNREACHABLE();
  }

private:
  Future<bool> authorized(
      const authorization::Request& request,
      const vector<GenericACL>& acls)
  {
    ACL::Entity subject;
    if (request.subject().has_value()) {
      subject.add_values(request.subject().value());
      subject.set_type(mesos::ACL::Entity::SOME);
    } else {
      subject.set_type(mesos::ACL::Entity::ANY);
    }

    ACL::Entity object;
    if (request.object().has_value()) {
      object.add_values(request.object().value());
      object.set_type(mesos::ACL::Entity::SOME);
    } else {
      object.set_type(mesos::ACL::Entity::ANY);
    }

    foreach (const GenericACL& acl, acls) {
      if (matches(subject, acl.subjects) &&
          matches(object, acl.objects)) {
        return allows(subject, acl.subjects) &&
            allows(object, acl.objects);
      }
    }

    return this->acls.permissive(); // None of the ACLs match.
  }

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
  bool matches(const ACL::Entity& request, const ACL::Entity& acl)
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
  bool allows(const ACL::Entity& request, const ACL::Entity& acl)
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

  ACLs acls;
};


Try<Authorizer*> LocalAuthorizer::create(const ACLs& acls)
{
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


LocalAuthorizer::LocalAuthorizer(const ACLs& acls)
    : process(new LocalAuthorizerProcess(acls))
{
  spawn(process);
}


LocalAuthorizer::~LocalAuthorizer()
{
  if (process != NULL) {
    terminate(process);
    wait(process);
    delete process;
  }
}


process::Future<bool> LocalAuthorizer::authorized(
  const authorization::Request& request)
{
  typedef Future<bool> (LocalAuthorizerProcess::*F)(
      const authorization::Request&);

  return dispatch(
      process,
      static_cast<F>(&LocalAuthorizerProcess::authorized),
      request);
}

} // namespace internal {
} // namespace mesos {
