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

#ifndef __SLAVE_COMPATIBILITY_HPP__
#define __SLAVE_COMPATIBILITY_HPP__

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include <mesos/mesos.pb.h>

namespace mesos {
namespace internal {
namespace slave {
namespace compatibility {

// This function checks whether `previous` and `current` are considered to be
// "equal", i.e., all fields in `SlaveInfo` are the same.
Try<Nothing> equal(
    const SlaveInfo& previous,
    const SlaveInfo& current);


// This function checks whether the changes between `previous` and `current`
// are considered to be "additive", according to the rules in the following
// table:
//
// Field      | Constraint
// -----------------------------------------------------------------------------
// hostname   | Must match exactly.
// port       | Must match exactly.
// domain     | Must either match exactly or change from not configured to
//            | configured.
// resources  | All previous resources must be present with the same type.
//            | For type SCALAR: The new value must be not smaller than the old.
//            | For type RANGE:  The new value must include the old ranges.
//            | For type SET:    The new value must be a superset of the old.
//            | New resources are permitted. In the presence of reservations,
//            | these rules are applied per role.
// attributes | All previous attributes must be present with the same type.
//            | For type SCALAR: The new value must be not smaller than the old.
//            | For type RANGE:  The new value must include the old ranges.
//            | For type TEXT:   The new value must exactly match the previous.
//            | New attributes are permitted.
//
Try<Nothing> additive(
    const SlaveInfo& previous,
    const SlaveInfo& current);

} // namespace compatibility {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_COMPATIBILITY_HPP__
