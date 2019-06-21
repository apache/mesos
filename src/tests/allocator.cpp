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
// limitations under the License

#include <string>

#include "tests/allocator.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace tests {

// This is a legacy helper where we take in a resource string
// and use that to set both quota guarantees and limits.
Quota2 createQuota(const string& role, const string& resources)
{
  Quota2 quota;
  quota.guarantees = CHECK_NOTERROR(ResourceQuantities::fromString(resources));
  quota.limits = CHECK_NOTERROR(ResourceLimits::fromString(resources));

  return quota;
}


WeightInfo createWeightInfo(const string& role, double weight)
{
  WeightInfo weightInfo;
  weightInfo.set_role(role);
  weightInfo.set_weight(weight);

  return weightInfo;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
