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

#ifndef __MASTER_QUOTA_HPP__
#define __MASTER_QUOTA_HPP__

#include <string>

#include <google/protobuf/repeated_field.h>

#include <mesos/mesos.hpp>

#include <mesos/quota/quota.hpp>

#include <stout/error.hpp>
#include <stout/hashset.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "master/registrar.hpp"
#include "master/registry.hpp"

namespace mesos {
namespace internal {
namespace master {
namespace quota {

// We do not impose any constraints upon quota registry operations.
// It is up to the master and allocator to determine whether a quota
// request is valid. Hence quota registry operations never fail (i.e.
// `perform()` never returns an `Error`). Note that this does not
// influence registry failures, e.g. a network partition may occur and
// will render the operation hanging (i.e. `Future` for the operation
// will not be set).

/**
 * Sets quota for a role. No assumptions are made here: the role may
 * be unknown to the master, or quota can be already set for the role.
 * If there is no quota stored for the role, a new entry is created,
 * otherwise an existing one is updated.
 */
class UpdateQuota : public RegistryOperation
{
public:
  // This operation needs to take in a `RepeatedPtrField` of `QuotaConfig`
  // to ensure all-or-nothing quota updates for multiple roles.
  explicit UpdateQuota(
      const google::protobuf::RepeatedPtrField<mesos::quota::QuotaConfig>&
        quotaConfigs);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  google::protobuf::RepeatedPtrField<mesos::quota::QuotaConfig> configs;
};


/**
 * Creates a `QuotaInfo` protobuf from the `QuotaRequest` protobuf.
 */
mesos::quota::QuotaInfo createQuotaInfo(
    const mesos::quota::QuotaRequest& request);

namespace validation {

// `QuotaInfo` is valid if the following conditions are met:
//   - Request includes a single role across all resources.
//   - Irrelevant fields in `Resources` are not set
//     (e.g. `ReservationInfo`).
//   - Request only contains scalar `Resources`.
//
// TODO(bmahler): Remove this in favor of `validate` below. This
// requires some new logic outside of this function to prevent
// users from setting `limit` explicitly in the old API and
// setting the `limit` implicitly for users of the old API before
// calling into this.
Option<Error> quotaInfo(const mesos::quota::QuotaInfo& quotaInfo);

} // namespace validation {

/**
 * A `QuotaRequest` is valid if the following conditions are met:
 *
 *   (1) The request has a valid non-"*" role.
 *
 *   (2) The guarantee and limit contain only valid non-revocable
 *       scalar resources without reservations, disk info, and
 *       only 1 entry for each resource name.
 *
 *   (3) If both guarantee and limit are set for a particular
 *       resource, then guarantee <= limit for that resource.
 *
 * TODO(bmahler): Remove the old validation function in favor of
 * this one. This requires some new logic outside of this function
 * to prevent users from setting `limit` explicitly in the old API
 * and setting the `limit` implicitly for users of the old API before
 * calling into this.
 */
Option<Error> validate(const mesos::quota::QuotaRequest& request);

/**
 * A `QuotaConfig` is valid if the following conditions are met:
 *
 *   (1) The config has a valid non-"*" role.
 *
 *   (2) Resource scalar values are non-negative and finite.
 *
 *   (3) If both guarantees and limits are set for a particular
 *       resource, then guarantee <= limit for that resource.
 */
Option<Error> validate(const mesos::quota::QuotaConfig& config);

} // namespace quota {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_QUOTA_HPP__
