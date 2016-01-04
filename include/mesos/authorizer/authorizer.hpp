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

#ifndef __MESOS_AUTHORIZER_AUTHORIZER_HPP__
#define __MESOS_AUTHORIZER_AUTHORIZER_HPP__

#include <iosfwd>
#include <string>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include <mesos/authorizer/authorizer.pb.h>

#include <process/future.hpp>

#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {

/**
 * An interface for authorization of actions with ACLs. Refer to
 * "authorizer.proto" and "docs/authorization.md" for the details
 * regarding the authorization mechanism.
 *
 * Each `authorize()` function returns `Future<bool>`, which has the same
 * meaning for all functions. If the action is allowed, the future is set to
 * `true`, otherwise to `false`. A third possible outcome is that the future
 * fails, which indicates that the request could not be completed at the
 * moment. This may be a temporary condition.
 *
 * NOTE: Any request allows bundling multiple values for each entity, which
 * are often principals. Though the default authorizer implementation
 * (`LocalAuthorizer`) supports this feature, Mesos code currently does not
 * authorize multiple principals in a single call.
 *
 * @see authorizer.proto
 * @see docs/authorization.md
 */
class Authorizer
{
public:
  static Try<Authorizer*> create(const std::string& name);

  virtual ~Authorizer() {}

  /**
   * Sets the Access Control Lists for the current instance of the interface.
   * The contents of the `acls` parameter are used to define the rules which
   * apply to the authorization actions.
   *
   * @param acls The access control lists used to initialize the authorizer
   *     instance. See "authorizer.proto" for a description of their format.
   *
   * @return `Nothing` if the instance of the authorizer was successfully
   *     initialized, an `Error` otherwise.
   *
   * TODO(arojas): This function is relevant for the default implementation
   * of the `Authorizer` class only (see MESOS-3072) and it will not be
   * called for any other implementation. Remove it once we have a module-only
   * initialization which relies on module-specific parameters supplied via
   * the modules JSON blob.
   */
  virtual Try<Nothing> initialize(const Option<ACLs>& acls) = 0;

  /**
   * Verifies whether a principal can register a framework with a specific role.
   *
   * @param request `ACL::RegisterFramework` packing all the parameters
   *     needed to verify if the given principal can register a framework
   *     with the specified role.
   *
   * @return true if the principal can register the framework with the
   *     specified role or false otherwise. A failed future indicates a
   *     problem processing the request; the request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::RegisterFramework& request) = 0;

  /**
   * Verifies whether a principal can run tasks as the given UNIX user.
   *
   * @param request `ACL::RunTask` packing all the parameters needed to verify
   *     if the given principal can launch a task using the specified UNIX user.
   *
   * @return true if the principal can run a task using the given UNIX user
   *     name, false otherwise. A failed future indicates a problem processing
   *     the request; the request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::RunTask& request) = 0;

  /**
   * Verifies whether a principal can shutdown a framework launched by another
   * principal.
   *
   * @param request `ACL::ShutdownFramework` packing all the parameters needed
   *     to verify the given principal can shutdown a framework originally
   *     registered by a (potentially different) framework principal.
   *
   * @return true if the principal can shutdown a framework registered by the
   *     framework principal, false otherwise. A failed future indicates a
   *     problem processing the request; the request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::ShutdownFramework& request) = 0;

  /**
   * Verifies whether a principal can reserve particular resources.
   *
   * @param request `ACL::ReserveResources` packing all the parameters needed to
   *     verify the given principal can reserve one or more types of resources.
   *
   * @return true if the principal can reserve the resources, false otherwise. A
   *     failed future indicates a problem processing the request; the request
   *     can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::ReserveResources& request) = 0;

  /**
   * Verifies whether a principal can unreserve resources reserved by another
   * principal.
   *
   * @param request `ACL::UnreserveResources` packing all the parameters needed
   *     to verify the given principal can unreserve resources which were
   *     reserved by the reserver principal contained in the request.
   *
   * @return true if the principal can unreserve resources which were reserved
   *     by the reserver principal, false otherwise. A failed future indicates
   *     a problem processing the request; the request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::UnreserveResources& request) = 0;

  /**
   * Verifies whether a principal can create a persistent volume.
   *
   * @param request `ACL::CreateVolume` packing all the parameters needed to
   *     verify the given principal can create the given type of volume.
   *
   * @return true if the principal can create a persistent volume, false
   *     otherwise. A failed future indicates a problem processing the
   *     request; the request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::CreateVolume& request) = 0;

  /**
   * Verifies whether a principal can destroy a volume created by another
   * principal.
   *
   * @param request `ACL::DestroyVolume` packing all the parameters needed to
   *     verify the given principal can destroy volumes which were created by
   *     the creator principal contained in the request.
   *
   * @return true if the principal can destroy volumes which were created by
   *     the creator principal, false otherwise. A failed future indicates a
   *     problem processing the request; the request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::DestroyVolume& request) = 0;

  /**
   * Verifies whether a principal can set a quota for a specific role.
   *
   * @param request `ACL::SetQuota` packing all the parameters needed to verify
   *     if the given principal can set a quota for the specified role.
   *
   * @return true if the principal can set a quota for the specified role,
   *     false otherwise. A failed future indicates a problem processing
   *     the request; the request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::SetQuota& request) = 0;

protected:
  Authorizer() {}
};


std::ostream& operator<<(std::ostream& stream, const ACLs& acls);

} // namespace mesos {

#endif // __MESOS_AUTHORIZER_AUTHORIZER_HPP__
