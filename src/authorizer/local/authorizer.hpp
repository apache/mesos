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

#ifndef __AUTHORIZER_AUTHORIZER_HPP__
#define __AUTHORIZER_AUTHORIZER_HPP__

#include <mesos/authorizer/authorizer.hpp>

#include <process/future.hpp>
#include <process/once.hpp>

#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {

// Forward declaration.
class LocalAuthorizerProcess;

// This Authorizer is constructed with all the required ACLs upfront.
class LocalAuthorizer : public Authorizer
{
public:
  // Creates a LocalAuthorizer.
  // Never returns a nullptr, instead sets the Try to error.
  //
  // This factory needs to return a raw pointer so its signature matches that
  // of tests::Module<T,N>::create() so typed tests can be performed.
  static Try<Authorizer*> create();

  virtual ~LocalAuthorizer();

  // Initialization is needed since this class is required to be default
  // constructible, however the ACLs still need to be provided. MESOS-3072
  // tries to address this requirement.
  virtual Try<Nothing> initialize(const Option<ACLs>& acls);

  // Implementation of Authorizer interface.
  virtual process::Future<bool> authorize(
      const ACL::RegisterFramework& request);
  virtual process::Future<bool> authorize(
      const ACL::RunTask& request);
  virtual process::Future<bool> authorize(
      const ACL::ShutdownFramework& request);
  virtual process::Future<bool> authorize(
      const ACL::ReserveResources& request);
  virtual process::Future<bool> authorize(
      const ACL::UnreserveResources& request);
  virtual process::Future<bool> authorize(
      const ACL::CreateVolume& request);
  virtual process::Future<bool> authorize(
      const ACL::DestroyVolume& request);
  virtual process::Future<bool> authorize(
      const ACL::SetQuota& request);

private:
  LocalAuthorizer();

  LocalAuthorizerProcess* process;

  process::Once initialized;
};

} // namespace internal {
} // namespace mesos {

#endif //__AUTHORIZER_AUTHORIZER_HPP__
