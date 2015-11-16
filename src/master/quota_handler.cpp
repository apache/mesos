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
* limitations under the License
*/

#include <mesos/resources.hpp>

#include <mesos/quota/quota.hpp>

#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/http.hpp>

#include <stout/protobuf.hpp>

#include "master/master.hpp"

namespace http = process::http;

using std::string;

using http::Accepted;
using http::BadRequest;
using http::Conflict;
using http::OK;

using process::Future;

using mesos::quota::QuotaInfo;
namespace mesos {
namespace internal {
namespace master {

Future<http::Response> Master::QuotaHandler::set(
    const http::Request& request) const
{
  // Authenticate and authorize the request.
  // TODO(alexr): Check Master::Http::authenticate() for an example.

  // TODO(alexr): Validate and convert the request to internal protobuf message.

  // Validate whether a quota request can be satisfied.
  // TODO(alexr): Implement as per MESOS-3073.

  // Populate master's quota-related local state. We do this before updating
  // the registry in order to make sure that we are not already trying to
  // satisfy a request for this role (since this is a multi-phase event).
  // NOTE: We do not need to remove quota for the role if the registry update
  // fails because in this case the master fails as well.
  QuotaInfo quotaInfo;
  master->quotas[quotaInfo.role()] = Quota{quotaInfo};

  // Update the registry with the new quota.
  // TODO(alexr): MESOS-3165.

  // We are all set, grant the request.
  // TODO(alexr): Implement as per MESOS-3073.
  // TODO(alexr): This should be done after registry operation succeeds.

  // Notfify allocator.
  master->allocator->setQuota(quotaInfo.role(), quotaInfo);

  return OK();
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
