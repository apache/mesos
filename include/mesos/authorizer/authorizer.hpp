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

#include <mesos/mesos.hpp>

#include <mesos/authorizer/acls.hpp>

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
  /**
   * Factory method used to create instances of authorizer which are loaded from
   * the `ModuleManager`. The parameters necessary to instantiate the authorizer
   * are taken from the contents of the `--modules` flag.
   *
   * @param name The name of the module to be loaded as registered in the
   *     `--modules` flag.
   *
   * @return An instance of `Authorizer*` if the module with the given name
   *     could be constructed. An error otherwise.
   */
  static Try<Authorizer*> create(const std::string &name);

  /**
   * Factory method used to create instances of the default 'local'  authorizer.
   *
   * @param acls The access control lists used to initialize the 'local'
   *     authorizer.
   *
   * @return An instance of the default 'local'  authorizer.
   */
  static Try<Authorizer*> create(const ACLs& acls);

  virtual ~Authorizer() {}

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
   * Verifies whether a principal can teardown a framework launched by another
   * principal.
   *
   * @param request `ACL::TeardownFramework` packing all the parameters needed
   *     to verify the given principal can teardown a framework originally
   *     registered by a (potentially different) framework principal.
   *
   * @return true if the principal can teardown a framework registered by the
   *     framework principal, false otherwise. A failed future indicates a
   *     problem processing the request; the request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::TeardownFramework& request) = 0;

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

  /**
   * Verifies whether a principal can remove a quota set by another principal.
   *
   * @param request `ACL::RemoveQuota` packing all the parameters needed to
   *     verify the given principal can remove quotas which were set by the
   *     principal contained in the set request.
   *
   * @return true if the principal can remove quotas which were set by the quota
   *     principal, false otherwise. A failed future indicates a problem
   *     processing the request; the request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::RemoveQuota& request) = 0;

  /**
   * Verifies whether a principal can update the weight for the specific roles.
   *
   * @param request `ACL::UpdateWeights` packing all the parameters needed to
   *     verify if the given principal is allowed to update the weight of the
   *     specified roles.
   *
   * @return true if the principal is allowed to update the weight for every
   *     specified role, false otherwise. A failed future indicates a problem
   *     processing the request; the request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::UpdateWeights& request) = 0;

protected:
  Authorizer() {}
};

} // namespace mesos {

#endif // __MESOS_AUTHORIZER_AUTHORIZER_HPP__
