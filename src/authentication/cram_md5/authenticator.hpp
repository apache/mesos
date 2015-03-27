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

#ifndef __AUTHENTICATION_CRAM_MD5_AUTHENTICATOR_HPP__
#define __AUTHENTICATION_CRAM_MD5_AUTHENTICATOR_HPP__

#include <string>

#include <mesos/module/authenticator.hpp>

#include <process/future.hpp>
#include <process/id.hpp>

#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace cram_md5 {

// Forward declaration.
class CRAMMD5AuthenticatorProcess;


class CRAMMD5Authenticator : public Authenticator
{
public:
  // Factory to allow for typed tests.
  static Try<Authenticator*> create();

  CRAMMD5Authenticator();

  virtual ~CRAMMD5Authenticator();

  virtual void initialize(const process::UPID& clientPid);

  // Returns the principal of the Authenticatee if successfully
  // authenticated otherwise None or an error. Note that we
  // distinguish authentication failure (None) from a failed future
  // in the event the future failed due to a transient error and
  // authentication can (should) be retried. Discarding the future
  // will cause the future to fail if it hasn't already completed
  // since we have already started the authentication procedure and
  // can't reliably cancel.
  virtual process::Future<Option<std::string>> authenticate();

private:
  CRAMMD5AuthenticatorProcess* process;
};


namespace secrets {

// Loads secrets (principal -> secret) into the in-memory auxiliary
// property plugin that is used by the authenticators.
void load(const std::map<std::string, std::string>& secrets);

void load(const Credentials& credentials);

} // namespace secrets {

} // namespace cram_md5 {
} // namespace internal {
} // namespace mesos {

#endif //__AUTHENTICATION_CRAM_MD5_AUTHENTICATOR_HPP__
