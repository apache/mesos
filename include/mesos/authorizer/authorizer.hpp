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

#ifndef __MESOS_AUTHORIZER_AUTHORIZER_HPP__
#define __MESOS_AUTHORIZER_AUTHORIZER_HPP__

#include <ostream>
#include <string>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include <mesos/authorizer/authorizer.pb.h>

#include <process/future.hpp>

#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {

/**
 * An interface used to provide authorization for actions with ACLs.
 * Refer to "docs/authorization.md" for the details regarding the
 * authorization mechanism.
 *
 * @see docs/authorization.md
 */
class Authorizer
{
public:
  static Try<Authorizer*> create(const std::string& name);

  virtual ~Authorizer() {}

  /**
   * Only relevant for the default implementation of the Authorizer class
   * (MESOS-3072) and it will not be called for any other implementation.
   *
   * Sets the Access Control Lists for the current instance of the interface.
   * The contents of the acls parameter are used to define the rules which apply
   * to the authorization actions. Please check the 'authorizer.proto' file to
   * review their format, and the 'docs/authorization.md' for a description on
   * how authorization is performed.
   *
   * @param acls The access control lists used to initialize the authorizer
   *     instance. See the autorizer.proto file for a description on their
   *     format.
   *
   * @return Nothing if the instance of the authorizer was successfully
   +     initialized, an Error otherwise.
   *
   * TODO(arojas): Remove once we have a module only initialization which would
   * rely only module specific parameters as supplied via the modules JSON blob
   * (see MESOS-3072).
   */
  virtual Try<Nothing> initialize(const Option<ACLs>& acls) = 0;

  /**
   * Used to verify if a principal is allowed to register a framework with a
   * specific role. The principal and role parameters are packed in the request.
   * If the principal is allowed to perform the action, this method returns
   * true. Otherwise it returns false. A third possible outcome is that the
   * future fails. Then it indicates the request could not be checked at the
   * moment. This may be a temporary condition.
   *
   * @param request An instance of an ACL::RegisterFramework protobuf message.
   *     It packs all the parameters needed to verify if the principal is
   *     allowed to register a framework with the specified role.
   *
   * @return true if the principal is allowed to register the framework with the
   *     specified role or false otherwise. A failed future is neither true
   *     nor false. It indicates a problem processing the request and the
   *     request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::RegisterFramework& request) = 0;

  /**
   * Used to verify if a principal is allowed to run tasks as the given UNIX
   * user. The principal and user parameters are packed in the request. If the
   * principal is allowed to perform the action, this method returns true.
   * Otherwise it returns false. A third possible outcome is that the future
   * fails. Then it indicates the request could not be checked at the moment.
   * This may be a temporary condition.
   *
   * @param request An instance of an ACL::RunTask protobuf message. It packs
   *     all the parameters needed to verify if a principal can launch a task
   *     using the specified UNIX user.
   *
   * @return true if the principal is allowed to run a task using the given
   *     UNIX user name, false otherwise. A failed future is neither true
   *     nor false. It indicates a problem processing the request and the
   *     request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::RunTask& request) = 0;

  /**
   * Used to verify if a principal is allowed to shut down a framework launched
   * by the given framework_principal. The principal and framework_principal
   * parameters are packed in the request. If the principal is allowed to
   * perform the action, this method returns true. Otherwise it returns false.
   * A third possible outcome is that the future fails. Then it indicates the
   * request could not be checked at the moment. This may be a temporary
   * condition.
   *
   * @param request An instance of an ACL::RunTask protobuf message. It packs
   *     all the parameters needed to verify the given principal is allowed to
   *     shut down a framework launched by the framework principal, i.e. the
   *     principal who originally registered the framework.
   *
   * @return true if the principal can shutdown a framework ran by the
   *     framework_principal, false otherwise. A failed future is neither true
   *     nor false. It indicates a problem processing the request and the
   *     request can be retried.
   */
  virtual process::Future<bool> authorize(
      const ACL::ShutdownFramework& request) = 0;

protected:
  Authorizer() {}
};


inline std::ostream& operator<<(std::ostream& stream, const ACLs& acls)
{
  return stream << acls.DebugString();
}

} // namespace mesos {

#endif // __MESOS_AUTHORIZER_AUTHORIZER_HPP__
