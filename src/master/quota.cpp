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

#include <stout/error.hpp>
#include <stout/option.hpp>

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


Try<QuotaInfo> createQuotaInfo(const QuotaRequest& request)
{
  return createQuotaInfo(request.role(), request.guarantee());
}


Try<QuotaInfo> createQuotaInfo(
    const string& role,
    const RepeatedPtrField<Resource>& resources)
{
  QuotaInfo quota;

  quota.set_role(role);
  quota.mutable_guarantee()->CopyFrom(resources);

  return quota;
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

  foreach (const Resource& resource, quotaInfo.guarantee()) {
    // Check that each guarantee/resource is valid.
    Option<Error> resourceError = Resources::validate(resource);
    if (resourceError.isSome()) {
      return Error(
          "QuotaInfo with invalid resource: " + resourceError->message);
    }

    // Check that `resource` does not contain fields that are
    // irrelevant for quota.

    if (resource.has_reservation()) {
      return Error("QuotaInfo must not contain ReservationInfo");
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

    // Check that the role is either unset or default.
    if (resource.has_role() && resource.role() != "*") {
      return Error("QuotaInfo resources must not specify a role");
    }
  }

  return None();
}

} // namespace validation {

} // namespace quota {
} // namespace master {
} // namespace internal {
} // namespace mesos {
