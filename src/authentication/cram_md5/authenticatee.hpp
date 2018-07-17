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

#ifndef __AUTHENTICATION_CRAM_MD5_AUTHENTICATEE_HPP__
#define __AUTHENTICATION_CRAM_MD5_AUTHENTICATEE_HPP__

#include <mesos/mesos.hpp>

#include <mesos/module/authenticatee.hpp>

#include <process/future.hpp>
#include <process/id.hpp>

#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace cram_md5 {

// Forward declaration.
class CRAMMD5AuthenticateeProcess;


class CRAMMD5Authenticatee : public Authenticatee
{
public:
  // Factory to allow for typed tests.
  static Try<Authenticatee*> create();

  CRAMMD5Authenticatee();

  ~CRAMMD5Authenticatee() override;

  process::Future<bool> authenticate(
      const process::UPID& pid,
      const process::UPID& client,
      const Credential& credential) override;

private:
  CRAMMD5AuthenticateeProcess* process;
};

} // namespace cram_md5 {
} // namespace internal {
} // namespace mesos {

#endif // __AUTHENTICATION_CRAM_MD5_AUTHENTICATEE_HPP__
