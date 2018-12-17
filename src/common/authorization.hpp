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

#ifndef __COMMON_AUTHORIZATION_HPP__
#define __COMMON_AUTHORIZATION_HPP__

#include <vector>

#include <mesos/authentication/authenticator.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <process/future.hpp>
#include <process/http.hpp>

#include <stout/hashset.hpp>
#include <stout/option.hpp>

namespace mesos {
namespace authorization {

extern hashset<std::string> AUTHORIZABLE_ENDPOINTS;

// Collects authorization results. Any discarded or failed future
// results in a failure; any false future results in 'false'.
process::Future<bool> collectAuthorizations(
    const std::vector<process::Future<bool>>& authorizations);

// Creates a `Subject` for authorization purposes when given an
// authenticated `Principal`. This function accepts and returns an
// `Option` to make call sites cleaner, since it is possible that
// `principal` will be `NONE`.
const Option<Subject> createSubject(
    const Option<process::http::authentication::Principal>& principal);

process::Future<bool> authorizeLogAccess(
    const Option<Authorizer*>& authorizer,
    const Option<process::http::authentication::Principal>& principal);

const process::http::authorization::AuthorizationCallbacks
  createAuthorizationCallbacks(Authorizer* authorizer);

} // namespace authorization {
} // namespace mesos {

#endif // __COMMON_AUTHORIZATION_HPP__
