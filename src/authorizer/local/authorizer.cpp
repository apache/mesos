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

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/protobuf.hpp>
#include <stout/try.hpp>

using process::Failure;
using process::Future;
using process::dispatch;

using std::string;

namespace mesos {
namespace internal {

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

  Future<bool> authorize(const ACL::RegisterFramework& request)
  {
    foreach (const ACL::RegisterFramework& acl, acls.register_frameworks()) {
      // ACL matches if both subjects and objects match.
      if (matches(request.principals(), acl.principals()) &&
          matches(request.roles(), acl.roles())) {
        // ACL is allowed if both subjects and objects are allowed.
        return allows(request.principals(), acl.principals()) &&
               allows(request.roles(), acl.roles());
      }
    }

    return acls.permissive(); // None of the ACLs match.
  }

  Future<bool> authorize(const ACL::RunTask& request)
  {
    foreach (const ACL::RunTask& acl, acls.run_tasks()) {
      // ACL matches if both subjects and objects match.
      if (matches(request.principals(), acl.principals()) &&
          matches(request.users(), acl.users())) {
        // ACL is allowed if both subjects and objects are allowed.
        return allows(request.principals(), acl.principals()) &&
               allows(request.users(), acl.users());
      }
    }

    return acls.permissive(); // None of the ACLs match.
  }

  Future<bool> authorize(const ACL::TeardownFramework& request)
  {
    foreach (const ACL::TeardownFramework& acl, acls.teardown_frameworks()) {
      // ACL matches if both subjects and objects match.
      if (matches(request.principals(), acl.principals()) &&
          matches(request.framework_principals(),
                  acl.framework_principals())) {
        // ACL is allowed if both subjects and objects are allowed.
        return allows(request.principals(), acl.principals()) &&
               allows(request.framework_principals(),
                      acl.framework_principals());
      }
    }

    return acls.permissive(); // None of the ACLs match.
  }

  Future<bool> authorize(const ACL::ReserveResources& request)
  {
    foreach (const ACL::ReserveResources& acl, acls.reserve_resources()) {
      // ACL matches if both subjects and objects match.
      if (matches(request.principals(), acl.principals()) &&
          matches(request.roles(), acl.roles())) {
        // ACL is allowed if both subjects and objects are allowed.
        return allows(request.principals(), acl.principals()) &&
               allows(request.roles(), acl.roles());
      }
    }

    return acls.permissive(); // None of the ACLs match.
  }

  Future<bool> authorize(const ACL::UnreserveResources& request)
  {
    foreach (const ACL::UnreserveResources& acl, acls.unreserve_resources()) {
      // ACL matches if both subjects and objects match.
      if (matches(request.principals(), acl.principals()) &&
          matches(request.reserver_principals(), acl.reserver_principals())) {
        // ACL is allowed if both subjects and objects are allowed.
        return allows(request.principals(), acl.principals()) &&
               allows(request.reserver_principals(), acl.reserver_principals());
      }
    }

    return acls.permissive(); // None of the ACLs match.
  }

  Future<bool> authorize(const ACL::CreateVolume& request)
  {
    foreach (const ACL::CreateVolume& acl, acls.create_volumes()) {
      // ACL matches if both subjects and objects match.
      if (matches(request.principals(), acl.principals()) &&
          matches(request.roles(), acl.roles())) {
        // ACL is allowed if both subjects and objects are allowed.
        return allows(request.principals(), acl.principals()) &&
               allows(request.roles(), acl.roles());
      }
    }

    return acls.permissive(); // None of the ACLs match.
  }

  Future<bool> authorize(const ACL::DestroyVolume& request)
  {
    foreach (const ACL::DestroyVolume& acl, acls.destroy_volumes()) {
      // ACL matches if both subjects and objects match.
      if (matches(request.principals(), acl.principals()) &&
          matches(request.creator_principals(), acl.creator_principals())) {
        // ACL is allowed if both subjects and objects are allowed.
        return allows(request.principals(), acl.principals()) &&
               allows(request.creator_principals(), acl.creator_principals());
      }
    }

    return acls.permissive(); // None of the ACLs match.
  }

  Future<bool> authorize(const ACL::SetQuota& request)
  {
    foreach (const ACL::SetQuota& acl, acls.set_quotas()) {
      // ACL matches if both subjects and objects match.
      if (matches(request.principals(), acl.principals()) &&
          matches(request.roles(), acl.roles())) {
        // ACL is allowed if both subjects and objects are allowed.
        return allows(request.principals(), acl.principals()) &&
               allows(request.roles(), acl.roles());
      }
    }

    return acls.permissive(); // None of the ACLs match.
  }

  Future<bool> authorize(const ACL::RemoveQuota& request)
  {
    foreach (const ACL::RemoveQuota& acl, acls.remove_quotas()) {
      // ACL matches if both subjects and objects match.
      if (matches(request.principals(), acl.principals()) &&
          matches(request.quota_principals(), acl.quota_principals())) {
        // ACL is allowed if both subjects and objects are allowed.
        return allows(request.principals(), acl.principals()) &&
               allows(request.quota_principals(), acl.quota_principals());
      }
    }

    return acls.permissive(); // None of the ACLs match.
  }

  Future<bool> authorize(const ACL::UpdateWeights& request)
  {
    foreach (const ACL::UpdateWeights& acl, acls.update_weights()) {
      // ACL matches if both subjects and objects match.
      if (matches(request.principals(), acl.principals()) &&
          matches(request.roles(), acl.roles())) {
        // ACL is allowed if both subjects and objects are allowed.
        return allows(request.principals(), acl.principals()) &&
               allows(request.roles(), acl.roles());
      }
    }

    return acls.permissive(); // None of the ACLs match.
  }

private:
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


Try<Authorizer*> LocalAuthorizer::create()
{
  Authorizer* local = new LocalAuthorizer;

  return local;
}


LocalAuthorizer::LocalAuthorizer() : process(NULL)
{
}


LocalAuthorizer::~LocalAuthorizer()
{
  if (process != NULL) {
    terminate(process);
    wait(process);
    delete process;
  }
}


Try<Nothing> LocalAuthorizer::initialize(const Option<ACLs>& acls)
{
  if (!acls.isSome()) {
    return Error("ACLs need to be specified for local authorizer");
  }

  if (!initialized.once()) {
    if (process != NULL) {
      return Error("Authorizer already initialized");
    }

    // Process initialization needs to be done here because default
    // implementations of modules need to be default constructible. So
    // actual construction is delayed until initialization.
    process = new LocalAuthorizerProcess(acls.get());
    spawn(process);

    initialized.done();
  }

  return Nothing();
}


Future<bool> LocalAuthorizer::authorize(const ACL::RegisterFramework& request)
{
  if (process == NULL) {
    return Failure("Authorizer not initialized");
  }

  // Necessary to disambiguate.
  typedef Future<bool>(LocalAuthorizerProcess::*F)(
      const ACL::RegisterFramework&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}


Future<bool> LocalAuthorizer::authorize(const ACL::RunTask& request)
{
  if (process == NULL) {
    return Failure("Authorizer not initialized");
  }

  // Necessary to disambiguate.
  typedef Future<bool>(LocalAuthorizerProcess::*F)(const ACL::RunTask&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}


Future<bool> LocalAuthorizer::authorize(const ACL::TeardownFramework& request)
{
  if (process == NULL) {
    return Failure("Authorizer not initialized");
  }

  // Necessary to disambiguate.
  typedef Future<bool>(LocalAuthorizerProcess::*F)(
      const ACL::TeardownFramework&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}


Future<bool> LocalAuthorizer::authorize(const ACL::ReserveResources& request)
{
  if (process == NULL) {
    return Failure("Authorizer not initialized");
  }

  // Necessary to disambiguate.
  typedef Future<bool>(
      LocalAuthorizerProcess::*F)(const ACL::ReserveResources&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}


Future<bool> LocalAuthorizer::authorize(const ACL::UnreserveResources& request)
{
  if (process == NULL) {
    return Failure("Authorizer not initialized");
  }

  // Necessary to disambiguate.
  typedef Future<bool>(
      LocalAuthorizerProcess::*F)(const ACL::UnreserveResources&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}


Future<bool> LocalAuthorizer::authorize(const ACL::CreateVolume& request)
{
  if (process == NULL) {
    return Failure("Authorizer not initialized");
  }

  // Necessary to disambiguate.
  typedef Future<bool>(LocalAuthorizerProcess::*F)(const ACL::CreateVolume&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}


Future<bool> LocalAuthorizer::authorize(const ACL::DestroyVolume& request)
{
  if (process == NULL) {
    return Failure("Authorizer not initialized");
  }

  // Necessary to disambiguate.
  typedef Future<bool>(LocalAuthorizerProcess::*F)(const ACL::DestroyVolume&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}


Future<bool> LocalAuthorizer::authorize(const ACL::SetQuota& request)
{
  if (process == NULL) {
    return Failure("Authorizer not initialized");
  }

  // Necessary to disambiguate.
  typedef Future<bool>(LocalAuthorizerProcess::*F)(const ACL::SetQuota&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}


Future<bool> LocalAuthorizer::authorize(const ACL::RemoveQuota& request)
{
  if (process == NULL) {
    return Failure("Authorizer not initialized");
  }

  // Necessary to disambiguate.
  typedef Future<bool>(LocalAuthorizerProcess::*F)(const ACL::RemoveQuota&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}


Future<bool> LocalAuthorizer::authorize(const ACL::UpdateWeights& request)
{
  if (process == NULL) {
    return Failure("Authorizer not initialized");
  }

  // Necessary to disambiguate.
  typedef Future<bool>(LocalAuthorizerProcess::*F)(const ACL::UpdateWeights&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}

} // namespace internal {
} // namespace mesos {
