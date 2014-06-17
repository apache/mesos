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

#include <string>
#include <vector>

#include <glog/logging.h>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/check.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include "authorizer/authorizer.hpp"

#include "mesos/mesos.hpp"

using process::Future;
using process::Owned;
using process::dispatch;

using std::string;
using std::vector;

namespace mesos {
namespace internal {

class LocalAuthorizerProcess : public ProtobufProcess<LocalAuthorizerProcess>
{
public:
  LocalAuthorizerProcess(const ACLs& _acls)
    : ProcessBase(process::ID::generate("authorizer")), acls(_acls) {}

  Future<bool> authorize(const ACL::RunTasks& request)
  {
    foreach (const ACL::RunTasks& acl, acls.run_tasks()) {
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

  Future<bool> authorize(const ACL::ReceiveOffers& request)
  {
    foreach (const ACL::ReceiveOffers& acl, acls.receive_offers()) {
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

  Future<bool> authorize(const ACL::HTTPGet& request)
  {
    foreach (const ACL::HTTPGet& acl, acls.http_get()) {
      // ACL matches if both subjects and objects match.
      if (matches(request.usernames(), acl.usernames()) &&
          matches(request.ips(), acl.ips()) &&
          matches(request.hostnames(), acl.hostnames()) &&
          matches(request.urls(), acl.urls())) {
        // ACL is allowed if both subjects and objects are allowed.
        return allows(request.usernames(), acl.usernames()) &&
               allows(request.ips(), acl.ips()) &&
               allows(request.hostnames(), acl.hostnames()) &&
               allows(request.urls(), acl.urls());
      }
    }

    return acls.permissive(); // None of the ACLs match.
  }

  Future<bool> authorize(const ACL::HTTPPut& request)
  {
    foreach (const ACL::HTTPPut& acl, acls.http_put()) {
      // ACL matches if both subjects and objects match.
      if (matches(request.usernames(), acl.usernames()) &&
          matches(request.ips(), acl.ips()) &&
          matches(request.hostnames(), acl.hostnames()) &&
          matches(request.urls(), acl.urls())) {
        // ACL is allowed if both subjects and objects are allowed.
        return allows(request.usernames(), acl.usernames()) &&
               allows(request.ips(), acl.ips()) &&
               allows(request.hostnames(), acl.hostnames()) &&
               allows(request.urls(), acl.urls());
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


Try<Owned<Authorizer> > Authorizer::create(const ACLs& acls)
{
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);

  if (authorizer.isError()) {
    return Error(authorizer.error());
  }

  Owned<LocalAuthorizer> authorizer_ = authorizer.get();
  return static_cast<Authorizer*>(authorizer_.release());
}


LocalAuthorizer::LocalAuthorizer(const ACLs& acls)
{
  process = new LocalAuthorizerProcess(acls);
  spawn(process);
}


LocalAuthorizer::~LocalAuthorizer()
{
  terminate(process);
  wait(process);
  delete process;
}


Try<Owned<LocalAuthorizer> > LocalAuthorizer::create(const ACLs& acls)
{
  // Validate ACLs.
  foreach (const ACL::HTTPGet& acl, acls.http_get()) {
    // At least one of the subjects should be set.
    if (acl.has_usernames() + acl.has_ips() + acl.has_hostnames() < 1) {
      return Error("At least one of the subjects should be set for ACL: " +
                    acl.DebugString());
    }
  }

  foreach (const ACL::HTTPPut& acl, acls.http_put()) {
    // At least one of the subjects should be set.
    if (acl.has_usernames() + acl.has_ips() + acl.has_hostnames() < 1) {
       return Error("At least one of the subjects should be set for ACL: " +
                     acl.DebugString());
     }
  }

  return new LocalAuthorizer(acls);
}


Future<bool> LocalAuthorizer::authorize(const ACL::RunTasks& request)
{
  // Necessary to disambiguate.
  typedef Future<bool>(LocalAuthorizerProcess::*F)(const ACL::RunTasks&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}


Future<bool> LocalAuthorizer::authorize(const ACL::ReceiveOffers& request)
{
  // Necessary to disambiguate.
  typedef Future<bool>(LocalAuthorizerProcess::*F)(const ACL::ReceiveOffers&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}


Future<bool> LocalAuthorizer::authorize(const ACL::HTTPGet& request)
{
  // Necessary to disambiguate.
  typedef Future<bool>(LocalAuthorizerProcess::*F)(const ACL::HTTPGet&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}


Future<bool> LocalAuthorizer::authorize(const ACL::HTTPPut& request)
{
  // Necessary to disambiguate.
  typedef Future<bool>(LocalAuthorizerProcess::*F)(const ACL::HTTPPut&);

  return dispatch(
      process, static_cast<F>(&LocalAuthorizerProcess::authorize), request);
}

} // namespace internal {
} // namespace mesos {
