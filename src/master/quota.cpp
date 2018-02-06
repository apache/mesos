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

#include "master/quota.hpp"

#include <string>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/roles.hpp>
#include <mesos/values.hpp>

#include <stout/error.hpp>
#include <stout/option.hpp>
#include <stout/set.hpp>

#include "common/resources_utils.hpp"

using google::protobuf::RepeatedPtrField;

using mesos::quota::QuotaInfo;
using mesos::quota::QuotaRequest;

using std::string;

namespace mesos {
namespace internal {
namespace master {
namespace quota {

UpdateQuota::UpdateQuota(const QuotaInfo& quotaInfo)
  : info(quotaInfo) {}


Try<bool> UpdateQuota::perform(
    Registry* registry,
    hashset<SlaveID>* /*slaveIDs*/)
{
  // If there is already quota stored for the role, update the entry.
  foreach (Registry::Quota& quota, *registry->mutable_quotas()) {
    if (quota.info().role() == info.role()) {
      quota.mutable_info()->CopyFrom(info);
      return true; // Mutation.
    }
  }

  // If there is no quota yet for the role, create a new entry.
  registry->add_quotas()->mutable_info()->CopyFrom(info);

  return true; // Mutation.
}


RemoveQuota::RemoveQuota(const string& _role) : role(_role) {}


Try<bool> RemoveQuota::perform(
    Registry* registry,
    hashset<SlaveID>* /*slaveIDs*/)
{
  // Remove quota for the role if a corresponding entry exists.
  for (int i = 0; i < registry->quotas().size(); ++i) {
    const Registry::Quota& quota = registry->quotas(i);

    if (quota.info().role() == role) {
      registry->mutable_quotas()->DeleteSubrange(i, 1);

      // NOTE: Multiple entries per role are not allowed.
      return true; // Mutation.
    }
  }

  return false;
}


namespace {

QuotaInfo createQuotaInfo(
    const string& role,
    const RepeatedPtrField<Resource>& guarantee,
    const RepeatedPtrField<Resource>& limit)
{
  QuotaInfo quota;

  quota.set_role(role);
  quota.mutable_guarantee()->CopyFrom(guarantee);
  quota.mutable_limit()->CopyFrom(limit);

  return quota;
}

} // namespace {


QuotaInfo createQuotaInfo(const QuotaRequest& request)
{
  return createQuotaInfo(request.role(), request.guarantee(), request.limit());
}


namespace validation {

Option<Error> quotaInfo(const QuotaInfo& quotaInfo)
{
  if (!quotaInfo.has_role()) {
    return Error("QuotaInfo must specify a role");
  }

  // Check the provided role is valid.
  Option<Error> roleError = roles::validate(quotaInfo.role());
  if (roleError.isSome()) {
    return Error("QuotaInfo with invalid role: " + roleError->message);
  }

  // Disallow quota for '*' role.
  // TODO(alexr): Consider allowing setting quota for '*' role, see MESOS-3938.
  if (quotaInfo.role() == "*") {
    return Error("QuotaInfo must not specify the default '*' role");
  }

  // Check that `QuotaInfo` contains at least one guarantee.
  // TODO(alexr): Relaxing this may make sense once quota is the
  // only way that we offer non-revocable resources. Setting quota
  // with an empty guarantee would mean the role is not entitled to
  // get non-revocable offers.
  if (quotaInfo.guarantee().empty()) {
    return Error("QuotaInfo with empty 'guarantee'");
  }

  hashset<string> names;

  foreach (const Resource& resource, quotaInfo.guarantee()) {
    // Check that `resource` does not contain fields that are
    // irrelevant for quota.
    if (resource.reservations_size() > 0) {
      return Error("QuotaInfo must not contain any ReservationInfo");
    }

    if (resource.has_disk()) {
      return Error("QuotaInfo must not contain DiskInfo");
    }

    if (resource.has_revocable()) {
      return Error("QuotaInfo must not contain RevocableInfo");
    }

    if (resource.type() != Value::SCALAR) {
      return Error("QuotaInfo must not include non-scalar resources");
    }

    // Check that resource names do not repeat.
    if (names.contains(resource.name())) {
      return Error("QuotaInfo contains duplicate resource name"
                   " '" + resource.name() + "'");
    }

    names.insert(resource.name());
  }

  // TODO(bmahler): We ensure the limit is not set here to enforce
  // that the v0 API and v1 SET_QUOTA Call cannot set an explicit
  // limit. Ideally this check would be done during call validation
  // but, at the time of writing this comment, call validation does
  // not validate the call type specific messages within `Call`.
  // Once we start to allow limit to be set via UPDATE_QUOTA, this
  // check will have to be moved into a SET_QUOTA validator and
  // a v0 /quota validator.
  if (quotaInfo.limit_size() > 0) {
    return Error("Setting QuotaInfo.limit is not supported via"
                 " /quota and the SET_QUOTA Call,"
                 "please use the UPDATE_QUOTA Call");
  }

  return None();
}

} // namespace validation {

Option<Error> validate(const QuotaRequest& request)
{
  if (!request.has_role()) {
    return Error("'QuotaRequest.role' must be set");
  }

  // Check the provided role is valid.
  Option<Error> error = roles::validate(request.role());
  if (error.isSome()) {
    return Error("Invalid 'QuotaRequest.role': " + error->message);
  }

  // Disallow quota for '*' role.
  //
  // TODO(alexr): Consider allowing setting quota for '*' role,
  // see MESOS-3938.
  if (request.role() == "*") {
    return Error("Invalid 'QuotaRequest.role': setting quota for the"
                 " default '*' role is not supported");
  }

  // Define a helper for use against guarantee and limit.
  auto validateQuotaResources = [](
      const RepeatedPtrField<Resource>& resources) -> Option<Error> {
    hashset<string> names;

    // Check that each resource does not contain fields
    // that are disallowed for quota resources.
    foreach (const Resource& resource, resources) {
      if (resource.has_reservation()) {
        return Error("'Resource.reservation' must not be set");
      }

      if (resource.reservations_size() > 0) {
        return Error("'Resource.reservations' must not be set");
      }

      if (resource.has_disk()) {
        return Error("'Resource.disk' must not be set");
      }

      if (resource.has_revocable()) {
        return Error("'Resource.revocable' must not be set");
      }

      if (resource.type() != Value::SCALAR) {
        return Error("'Resource.type' must be 'SCALAR'");
      }

      if (resource.has_shared()) {
        return Error("'Resource.shared' must not be set");
      }

      if (resource.has_provider_id()) {
        return Error("'Resource.provider_id' must not be set");
      }

      // Check that resource names do not repeat.
      if (names.contains(resource.name())) {
        return Error("Duplicate '" + resource.name() + "'; only a single"
                     " entry for each resource is supported");
      }

      names.insert(resource.name());
    }

    // Finally, ensure the resources are valid.
    return Resources::validate(resources);
  };

  error = validateQuotaResources(request.guarantee());
  if (error.isSome()) {
    return Error("Invalid 'QuotaRequest.guarantee': " + error->message);
  }

  error = validateQuotaResources(request.limit());
  if (error.isSome()) {
    return Error("Invalid 'QuotaRequest.limit': " + error->message);
  }

  // Validate that guarantee <= limit.
  Resources guarantee = request.guarantee();
  Resources limit = request.limit();

  // This needs to be checked on a per-resource basis for those
  // resources that are specified in both the guarantee and limit.
  foreach (const string& name, guarantee.names() & limit.names()) {
    Value::Scalar guaranteeScalar =
      CHECK_NOTNONE(guarantee.get<Value::Scalar>(name));
    Value::Scalar limitScalar =
      CHECK_NOTNONE(limit.get<Value::Scalar>(name));

    if (!(guaranteeScalar <= limitScalar)) {
      return Error("'QuotaRequest.guarantee' (" + stringify(guarantee) + ")"
                     " is not contained within the 'QuotaRequest.limit'"
                     " (" + stringify(limit) + ")");
    }
  }

  return None();
}

} // namespace quota {
} // namespace master {
} // namespace internal {
} // namespace mesos {
