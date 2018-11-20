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

#include "common/authorization.hpp"

#include <algorithm>
#include <string>

#include <mesos/mesos.hpp>

#include <process/collect.hpp>

#include <stout/foreach.hpp>
#include <stout/none.hpp>

using std::string;
using std::vector;

using process::Future;

using process::http::authentication::Principal;

namespace mesos {
namespace authorization {

Future<bool> collectAuthorizations(const vector<Future<bool>>& authorizations)
{
  return process::collect(authorizations)
    .then([](const vector<bool>& results) -> Future<bool> {
      return std::find(results.begin(), results.end(), false) == results.end();
    });
}


const Option<Subject> createSubject(const Option<Principal>& principal)
{
  if (principal.isSome()) {
    Subject subject;

    if (principal->value.isSome()) {
      subject.set_value(principal->value.get());
    }

    foreachpair (const string& key, const string& value, principal->claims) {
      Label* claim = subject.mutable_claims()->mutable_labels()->Add();
      claim->set_key(key);
      claim->set_value(value);
    }

    return subject;
  }

  return None();
}


Future<bool> authorizeLogAccess(
    const Option<Authorizer*>& authorizer,
    const Option<Principal>& principal)
{
  if (authorizer.isNone()) {
    return true;
  }

  Request request;
  request.set_action(ACCESS_MESOS_LOG);

  Option<Subject> subject = createSubject(principal);
  if (subject.isSome()) {
    request.mutable_subject()->CopyFrom(subject.get());
  }

  return authorizer.get()->authorized(request);
}

} // namespace authorization {
} // namespace mesos {
