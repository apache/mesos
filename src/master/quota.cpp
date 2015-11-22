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

#include "master/quota.hpp"

#include <string>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <stout/error.hpp>
#include <stout/option.hpp>

using mesos::quota::QuotaInfo;

using std::string;

namespace mesos {
namespace internal {
namespace master {
namespace quota {

UpdateQuota::UpdateQuota(const mesos::quota::QuotaInfo& quotaInfo)
  : info(quotaInfo) {}


Try<bool> UpdateQuota::perform(
    Registry* registry,
    hashset<SlaveID>*,
    bool)
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


RemoveQuota::RemoveQuota(const std::string& _role) : role(_role) {}


Try<bool> RemoveQuota::perform(
    Registry* registry,
    hashset<SlaveID>*,
    bool)
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


namespace validation {

Try<Nothing> quotaInfo(const QuotaInfo& quotaInfo)
{
  // The reference role for the quota request.
  string role = quotaInfo.role();

  // Check that QuotaInfo contains at least one guarantee.
  if (quotaInfo.guarantee().size() == 0) {
    return Error("QuotaInfo with empty 'guarantee'");
  }

  foreach (const Resource& resource, quotaInfo.guarantee()) {
    // Check that each guarantee/resource is valid.
    Option<Error> error = Resources::validate(resource);
    if (error.isSome()) {
      return Error(
          "QuotaInfo with invalid resource: " + error.get().message);
    }

    // Check that `Resource` does not contain non-relevant fields for quota.

    if (resource.has_reservation()) {
      return Error("QuotaInfo may not contain ReservationInfo");
    }
    if (resource.has_disk()) {
      return Error("QuotaInfo may not contain DiskInfo");
    }
    if (resource.has_revocable()) {
      return Error("QuotaInfo may not contain RevocableInfo");
    }

    // Check that the `Resource` is scalar.
    if (resource.type() != Value::SCALAR) {
      return Error(
          "QuotaInfo may not include non-scalar resources");
    }

    // Check that all roles are set and equal.

    if (!resource.has_role()) {
      return Error("QuotaInfo resources must specify a role");
    }
    if (resource.role().empty()) {
      return Error("QuotaInfo resources must specify a non-empty role");
    }

    if (role.empty()) {
      // Store first encountered role as reference.
      role = resource.role();
    } else if (role != resource.role()) {
      // All roles should be equal across a quota request.
      return Error("Quota request with different roles: '" + role +
                   "', '" + resource.role() + "'");
    }
  }

  return Nothing();
}

} // namespace validation {

} // namespace quota {
} // namespace master {
} // namespace internal {
} // namespace mesos {
