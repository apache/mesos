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

#include "master/master.hpp"

#include <mesos/resources.hpp>

#include <mesos/quota/quota.hpp>

#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/http.hpp>

#include <stout/protobuf.hpp>

#include "logging/logging.hpp"

#include "master/quota.hpp"

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

static Try<google::protobuf::RepeatedPtrField<Resource>> parseResources(
    const std::string& _resources)
{
  Try<JSON::Array> parse = JSON::parse<JSON::Array>(_resources);
  if (parse.isError()) {
    return Error("Error in parsing 'resources' string ('" + _resources +
                 "'): " + parse.error());
  }

  // Create Protobuf representation of resources.
  Try<google::protobuf::RepeatedPtrField<Resource>> resources =
    ::protobuf::parse<google::protobuf::RepeatedPtrField<Resource>>(
        parse.get());

  if (resources.isError()) {
    return Error(
        "Error in parsing 'resources' JSON array: " + resources.error());
  }

  return resources.get();
}

// Creates a `QuotaInfo` protobuf from the quota request.
static Try<QuotaInfo> createQuotaInfo(
    const google::protobuf::RepeatedPtrField<Resource>& resources)
{
  VLOG(1) << "Constructing QuotaInfo from resources protobuf";

  QuotaInfo quota;
  quota.mutable_guarantee()->CopyFrom(resources);

  // Set the role if we have one.
  if (resources.size() > 0) {
     quota.set_role(resources.begin()->role());
  }

  return quota;
}


Future<http::Response> Master::QuotaHandler::set(
    const http::Request& request) const
{
  VLOG(1) << "Setting quota from request: '" << request.body << "'";

  // Authenticate and authorize the request.
  // TODO(alexr): Check Master::Http::authenticate() for an example.

  // Check that the request type is POST which is guaranteed by the master.
  CHECK_EQ("POST", request.method);

  // Validate request and extract JSON.

  Try<hashmap<string, string>> decode = http::query::decode(request.body);
  if (decode.isError()) {
    return BadRequest("Failed to decode set quota request query string ('" +
                      request.body + "'): " +
                      decode.error());
  }

  hashmap<string, string> values = decode.get();

  if (!values.contains("resources")) {
    return BadRequest("Failed to parse set quota request query string ('" +
                      request.body + "'): Missing 'resources'");
  }

  Try<google::protobuf::RepeatedPtrField<Resource>> resources =
    parseResources(values["resources"]);

  if (resources.isError()) {
    return BadRequest("Failed to parse set quota request query string ('" +
                      request.body + "'): " + resources.error());
  }

  // Create the `QuotaInfo` protobuf message from the request JSON.
  Try<QuotaInfo> create = createQuotaInfo(resources.get());
  if (create.isError()) {
    return BadRequest("Failed to create QuotaInfo from set quota request "
                      "query string '(" + request.body + "'): " +
                      create.error());
  }

  // Check that the `QuotaInfo` is a valid quota request.
  Try<Nothing> validate = quota::validation::quotaInfo(create.get());
  if (validate.isError()) {
    return BadRequest("Failed to validate set quota request query string: ('" +
                      request.body + "'): " + validate.error());
  }

  // Check that the role is known by the master.
  // TODO(alexr): Once we are able to dynamically add roles, we should stop
  // checking whether the requested role is known to the master, because an
  // operator may set quota for a role that is about to be introduced.
  if (!master->roles.contains(create.get().role())) {
    return BadRequest("Failed to validate set quota request query string: ('" +
                      request.body +"')': Unknown role: '" +
                      create.get().role() + "'");
  }

  // Check that we are not updating an existing quota.
  // TODO(joerg84): Update error message once quota update is in place.
  if (master->quotas.contains(create.get().role())) {
    return BadRequest("Failed to validate set quota request query string: ('" +
                      request.body + "')': "
                      "Can not set quota for a role that already has quota");
  }

  const QuotaInfo& quotaInfo = create.get();

  // Validate whether a quota request can be satisfied.
  // TODO(alexr): Implement as per MESOS-3073.

  // Populate master's quota-related local state. We do this before updating
  // the registry in order to make sure that we are not already trying to
  // satisfy a request for this role (since this is a multi-phase event).
  // NOTE: We do not need to remove quota for the role if the registry update
  // fails because in this case the master fails as well.
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
