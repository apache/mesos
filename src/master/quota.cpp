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

#include "common/resource_quantities.hpp"
#include "common/resources_utils.hpp"
#include "common/validation.hpp"

using google::protobuf::Map;
using google::protobuf::RepeatedPtrField;

using mesos::quota::QuotaConfig;
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
    const RepeatedPtrField<Resource>& guarantee)
{
  QuotaInfo quota;

  quota.set_role(role);
  quota.mutable_guarantee()->CopyFrom(guarantee);

  return quota;
}

} // namespace {


QuotaInfo createQuotaInfo(const QuotaRequest& request)
{
  return createQuotaInfo(request.role(), request.guarantee());
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

  return None();
}

} // namespace validation {

Option<Error> validate(const QuotaConfig& config)
{
  if (!config.has_role()) {
    return Error("'QuotaConfig.role' must be set");
  }

  // Check the provided role is valid.
  Option<Error> error = roles::validate(config.role());
  if (error.isSome()) {
    return Error("Invalid 'QuotaConfig.role': " + error->message);
  }

  // Disallow quota for '*' role.
  if (config.role() == "*") {
    return Error(
      "Invalid 'QuotaConfig.role': setting quota for the"
      " default '*' role is not supported");
  }

  // Validate scalar values.
  foreach (auto&& guarantee, config.guarantees()) {
    Option<Error> error =
      common::validation::validateInputScalarValue(guarantee.second.value());

    if (error.isSome()) {
      return Error(
          "Invalid guarantee configuration {'" + guarantee.first + "': " +
          stringify(guarantee.second) + "}: " + error->message);
    }
  }

  foreach (auto&& limit, config.limits()) {
    Option<Error> error =
      common::validation::validateInputScalarValue(limit.second.value());

    if (error.isSome()) {
      return Error(
          "Invalid limit configuration {'" + limit.first + "': " +
          stringify(limit.second) + "}: " + error->message);
    }
  }

  // Validate guarantees <= limits.
  ResourceLimits limits{config.limits()};
  ResourceQuantities guarantees{config.guarantees()};

  if (!limits.contains(guarantees)) {
    return Error(
        "'QuotaConfig.guarantees' " + stringify(config.guarantees()) +
        " is not contained within the 'QuotaConfig.limits' " +
        stringify(config.limits()));
  }

  return None();
}

} // namespace quota {
} // namespace master {
} // namespace internal {
} // namespace mesos {
