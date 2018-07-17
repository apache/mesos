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

#ifndef __MASTER_WEIGHTS_HPP__
#define __MASTER_WEIGHTS_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <stout/error.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "master/registrar.hpp"
#include "master/registry.hpp"

namespace mesos {
namespace internal {
namespace master {
namespace weights {

// We do not impose any constraints upon weights registry operations.
// It is up to the master to determine whether a weights update
// request is valid. Hence weights registry operations never fail (i.e.
// `perform()` never returns an `Error`). Note that this does not
// influence registry failures, e.g. a network partition may occur and
// will render the operation hanging (i.e. `Future` for the operation
// will not be set).

/**
 * Updates weights for the specified roles. No assumptions are made here:
 * the roles may be unknown to the master, or weights can be already set
 * for the roles. If there are no weights stored for the roles, some new
 * entries are created, otherwise the existing entries are updated.
 */
class UpdateWeights : public RegistryOperation
{
public:
  explicit UpdateWeights(const std::vector<WeightInfo>& _weightInfos);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  const std::vector<WeightInfo> weightInfos;
};

} // namespace weights {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_WEIGHTS_HPP__
