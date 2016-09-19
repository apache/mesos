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
 * otherwise an existing one is updated. This operation always mutates
 * the registry.
 *
 * TODO(alexr): Introduce equality operator in `Registry::Quota` or
 * `QuotaInfo` to avoid mutation in case of update to an equal value.
 * However, even if we return `false` (i.e. no mutation), the current
 * implementation of the registrar will still save the object again.
 */
class UpdateQuota : public Operation
{
public:
  explicit UpdateQuota(const mesos::quota::QuotaInfo& quotaInfo);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs);

private:
  const mesos::quota::QuotaInfo info;
};


/**
 * Removes quota for a role. If there is no quota stored for the role,
 * no action is performed.
 *
 * TODO(alexr): Consider uniting this operation with `UpdateQuota`.
 */
class RemoveQuota : public Operation
{
public:
  explicit RemoveQuota(const std::string& _role);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs);

private:
  const std::string role;
};


/**
 * Creates a `QuotaInfo` protobuf from the `QuotaRequest` protobuf.
 */
Try<mesos::quota::QuotaInfo> createQuotaInfo(
    const mesos::quota::QuotaRequest& request);

/**
 * Creates a `QuotaInfo` protobuf from its components.
 */
Try<mesos::quota::QuotaInfo> createQuotaInfo(
    const std::string& role,
    const google::protobuf::RepeatedPtrField<Resource>& resources);


namespace validation {

// `QuotaInfo` is valid if the following conditions are met:
//   - Request includes a single role across all resources.
//   - Irrelevant fields in `Resources` are not set
//     (e.g. `ReservationInfo`).
//   - Request only contains scalar `Resources`.
Option<Error> quotaInfo(const mesos::quota::QuotaInfo& quotaInfo);

} // namespace validation {

} // namespace quota {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_QUOTA_HPP__
