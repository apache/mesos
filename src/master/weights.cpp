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

#include "master/weights.hpp"

namespace mesos {
namespace internal {
namespace master {
namespace weights {

UpdateWeights::UpdateWeights(const std::vector<WeightInfo>& _weightInfos)
  : weightInfos(_weightInfos) {}


Try<bool> UpdateWeights::perform(
    Registry* registry,
    hashset<SlaveID>* /*slaveIDs*/)
{
  bool mutated = false;
  if (weightInfos.empty()) {
    return false; // No mutation.
  }

  foreach (const WeightInfo& weightInfo, weightInfos) {
    bool hasStored = false;
    for (int i = 0; i < registry->weights().size(); ++i) {
      Registry::Weight* weight = registry->mutable_weights(i);

      if (weight->info().role() != weightInfo.role()) {
        continue;
      }

      hasStored = true;

      // If there is already weight stored for the specified role
      // and also its value is changed, update the entry.
      if (weight->info().weight() != weightInfo.weight()) {
        weight->mutable_info()->CopyFrom(weightInfo);
        mutated = true;
      }

      break;
    }

    // If there is no weight yet for this role in registry,
    // create a new entry.
    if (!hasStored) {
      registry->add_weights()->mutable_info()->CopyFrom(weightInfo);
      mutated = true;
    }
  }

  return mutated;
}

} // namespace weights {
} // namespace master {
} // namespace internal {
} // namespace mesos {
