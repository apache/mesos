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

#include <glog/logging.h>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/json.hpp>
#include <stout/try.hpp>

#include "mesos/mesos.hpp"

namespace mesos {

// Forward declaration.
class LocalAuthorizerProcess;


class Authorizer
{
public:
  virtual ~Authorizer() {}

  // Attempts to create an Authorizer based on the ACLs.
  static Try<process::Owned<Authorizer> > create(const ACLs& acls);

  // Returns true if the ACL can be satisfied or false otherwise.
  // A failed future indicates a transient failure and the user
  // can (should) retry.
  virtual process::Future<bool> authorize(
      const ACL::RegisterFramework& request) = 0;
  virtual process::Future<bool> authorize(
      const ACL::RunTask& request) = 0;
  virtual process::Future<bool> authorize(
      const ACL::ShutdownFramework& request) = 0;

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
  virtual process::Future<bool> authorize(
      const ACL::RegisterFramework& request);
  virtual process::Future<bool> authorize(
      const ACL::RunTask& request);
  virtual process::Future<bool> authorize(
      const ACL::ShutdownFramework& request);

private:
  LocalAuthorizer(const ACLs& acls);
  LocalAuthorizerProcess* process;
};

} // namespace mesos {

#endif //__AUTHORIZER_AUTHORIZER_HPP__
