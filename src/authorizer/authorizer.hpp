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

#ifndef __AUTHORIZER_AUTHORIZER_HPP__
#define __AUTHORIZER_AUTHORIZER_HPP__

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

#include "mesos/mesos.hpp"

namespace mesos {
namespace internal {

// Forward declaration.
class LocalAuthorizerProcess;


class Authorizer
{
public:
  virtual ~Authorizer() {}

  // Attempts to create an Authorizer based on the ACLs.
  static Try<process::Owned<Authorizer> > create(const JSON::Object& acls);

  // Returns true if the ACL can be satisfied or false otherwise.
  // A failed future indicates a transient failure and the user
  // can (should) retry.
  virtual process::Future<bool> authorize(
      const ACL::RunTasks& request) = 0;
  virtual process::Future<bool> authorize(
      const ACL::ReceiveOffers& request) = 0;
  virtual process::Future<bool> authorize(
      const ACL::HTTPGet& request) = 0;
  virtual process::Future<bool> authorize(
      const ACL::HTTPPut& request) = 0;

protected:
  Authorizer() {}
};


// This Authorizer is constructed with all the required ACLs upfront.
class LocalAuthorizer : public Authorizer
{
public:
  // Validates the ACLs and creates a LocalAuthorizer.
  static Try<process::Owned<LocalAuthorizer> > create(const ACLs& acls);
  virtual ~LocalAuthorizer();

  // Implementation of Authorizer interface.
  virtual process::Future<bool> authorize(const ACL::RunTasks& request);
  virtual process::Future<bool> authorize(const ACL::ReceiveOffers& request);
  virtual process::Future<bool> authorize(const ACL::HTTPGet& request);
  virtual process::Future<bool> authorize(const ACL::HTTPPut& request);

private:
  LocalAuthorizer(const ACLs& acls);
  LocalAuthorizerProcess* process;
};


class LocalAuthorizerProcess : public ProtobufProcess<LocalAuthorizerProcess>
{
public:
  LocalAuthorizerProcess(const ACLs& _acls)
    : ProcessBase(process::ID::generate("authorizer")), acls(_acls) {}

  process::Future<bool> authorize(const ACL::RunTasks& request)
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

  process::Future<bool> authorize(const ACL::ReceiveOffers& request)
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

  process::Future<bool> authorize(const ACL::HTTPGet& request)
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

  process::Future<bool> authorize(const ACL::HTTPPut& request)
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
      foreach (const std::string& value, request.values()) {
        bool found = false;
        foreach (const std::string& value_, acl.values()) {
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
      foreach (const std::string& value, request.values()) {
        bool found = false;
        foreach (const std::string& value_, acl.values()) {
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


Try<process::Owned<Authorizer> > Authorizer::create(const JSON::Object& acls_)
{
  // Convert ACLs from JSON to Protobuf.
  Try<ACLs> acls = protobuf::parse<ACLs>(acls_);
  if (acls.isError()) {
    return Error("Invalid ACLs format: " + acls.error());
  }

  Try<process::Owned<LocalAuthorizer> > authorizer =
    LocalAuthorizer::create(acls.get());

  if (authorizer.isError()) {
    return Error(authorizer.error());
  }

  process::Owned<LocalAuthorizer> authorizer_ = authorizer.get();
  return static_cast<Authorizer*>(authorizer_.release());
}


LocalAuthorizer::LocalAuthorizer(const ACLs& acls)
{
  process = new LocalAuthorizerProcess(acls);
  process::spawn(process);
}


LocalAuthorizer::~LocalAuthorizer()
{
  process::terminate(process);
  process::wait(process);
  delete process;
}


Try<process::Owned<LocalAuthorizer> > LocalAuthorizer::create(const ACLs& acls)
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


process::Future<bool> LocalAuthorizer::authorize(
    const ACL::RunTasks& request)
{
  // Necessary to disambiguate.
  typedef process::Future<bool>(LocalAuthorizerProcess::*F)(
      const ACL::RunTasks&);

  return process::dispatch(
      process,
      static_cast<F>(&LocalAuthorizerProcess::authorize),
      request);
}


process::Future<bool> LocalAuthorizer::authorize(
    const ACL::ReceiveOffers& request)
{
  // Necessary to disambiguate.
  typedef process::Future<bool>(LocalAuthorizerProcess::*F)(
      const ACL::ReceiveOffers&);

  return process::dispatch(
      process,
      static_cast<F>(&LocalAuthorizerProcess::authorize),
      request);
}


process::Future<bool> LocalAuthorizer::authorize(
    const ACL::HTTPGet& request)
{
  // Necessary to disambiguate.
  typedef process::Future<bool>(LocalAuthorizerProcess::*F)(
      const ACL::HTTPGet&);

  return process::dispatch(
      process,
      static_cast<F>(&LocalAuthorizerProcess::authorize),
      request);
}


process::Future<bool> LocalAuthorizer::authorize(
    const ACL::HTTPPut& request)
{
  // Necessary to disambiguate.
  typedef process::Future<bool>(LocalAuthorizerProcess::*F)(
      const ACL::HTTPPut&);

  return process::dispatch(
      process,
      static_cast<F>(&LocalAuthorizerProcess::authorize),
      request);
}

} // namespace internal {
} // namespace mesos {

#endif //__AUTHORIZER_AUTHORIZER_HPP__
