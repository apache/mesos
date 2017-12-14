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

#include <string>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <stout/error.hpp>
#include <stout/try.hpp>

#include "tests/resources_utils.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace tests {

Resources allocatedResources(
    const Resources& resources,
    const string& role)
{
  Resources result = resources;
  result.allocate(role);
  return result;
}


Resource createPorts(const ::mesos::Value::Ranges& ranges)
{
  Value value;
  value.set_type(Value::RANGES);
  value.mutable_ranges()->CopyFrom(ranges);

  Resource resource;
  resource.set_name("ports");
  resource.set_type(Value::RANGES);
  resource.mutable_ranges()->CopyFrom(value.ranges());

  return resource;
}


Try<::mesos::Value::Ranges> fragment(
    const ::mesos::Value::Range& bounds,
    size_t numRanges)
{
  uint64_t numValues = bounds.end() - bounds.begin() + 1;

  // Compute the max number of ranges.
  //
  // If `numValues` is even, then the maximum number of ranges is
  // `numValues / 2`:
  //   [1-2] -> 2 values, maximum 1 range:  [1-2]
  //   [1-4] -> 4 values, maximum 2 ranges: [1-1,3-4]
  //   [1-6] -> 6 values, maximum 3 ranges: [1-1,3-3,5-6]
  //
  // If `numValues` is odd, then the maximum number of ranges is
  // `(numValues + 1) / 2`:
  //   [1-1] -> 1 values, maximum 1 range:  [1-1]
  //   [1-3] -> 3 values, maximum 2 ranges: [1-1,3-3]
  //   [1-5] -> 5 values, maximum 3 ranges: [1-1,3-3,5-5]
  //
  uint64_t maxRanges;
  if (numValues % 2 == 0) {
    maxRanges = numValues / 2;
  } else {
    maxRanges = (numValues + 1) / 2;
  }

  if (numRanges > maxRanges) {
    return Error("Requested more distinct ranges than possible");
  }

  // See the documentation above for the fragmentation technique.
  // We fragment from the front of the bounds until we have the
  // desired number of ranges.
  ::mesos::Value::Ranges ranges;
  ranges.mutable_range()->Reserve(static_cast<int>(numRanges));

  for (size_t i = 0; i < numRanges; ++i) {
    Value::Range* range = ranges.add_range();

    range->set_begin(bounds.begin() + (i * 2));
    range->set_end(range->begin());
  }

  // Make sure the last range covers the end of the bounds.
  if (!ranges.range().empty()) {
    ranges.mutable_range()->rbegin()->set_end(bounds.end());
  }

  return ranges;
}


::mesos::Value::Range createRange(uint64_t begin, uint64_t end)
{
  ::mesos::Value::Range range;
  range.set_begin(begin);
  range.set_end(end);
  return range;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
