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

#ifndef __TESTS_RESOURCES_UTILS_HPP__
#define __TESTS_RESOURCES_UTILS_HPP__

#include <string>
#include <ostream>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace tests {

// Returns a copy of the resources that are allocated to the role.
//
// TODO(bmahler): Consider adding a top-level `AllocatedResources`
// that has the invariant that all resources contained within it
// have an `AllocationInfo` set. This class could prevent
// malformed operations between `Resources` and
// `AllocatedResources`, and could clarify interfaces that take
// allocated resources (e.g. allocator, sorter, etc).
Resources allocatedResources(
    const Resources& resources,
    const std::string& role);


// Creates a "ports(*)" resource for the given ranges.
Resource createPorts(const ::mesos::Value::Ranges& ranges);

// Fragments the given range bounds into a number of subranges.
// Returns an Error if 'numRanges' is too large. E.g.
//
//   [1-10], 1 -> [1-10]
//   [1-10], 2 -> [1-1,3-10]
//   [1-10], 3 -> [1-1,3-3,5-10]
//   [1-10], 4 -> [1-1,3-3,5-5,7-10]
//   [1-10], 5 -> [1-1,3-3,5-5,7-7,9-10]
//   [1-10], 6 -> Error
//
Try<::mesos::Value::Ranges> fragment(
    const ::mesos::Value::Range& bounds,
    size_t numRanges);

::mesos::Value::Range createRange(uint64_t begin, uint64_t end);

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_RESOURCES_UTILS_HPP__
