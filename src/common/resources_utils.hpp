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

#ifndef __RESOURCES_UTILS_HPP__
#define __RESOURCES_UTILS_HPP__

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <stout/try.hpp>

namespace mesos {

// Tests if the given Resource needs to be checkpointed on the slave.
// NOTE: We assume the given resource is validated.
bool needCheckpointing(const Resource& resource);

// Returns the total resources by applying the given checkpointed
// resources to the given resources. This function is useful when we
// want to calculate the total resources of a slave from the resources
// specified from the command line and the checkpointed resources.
// Returns error if the given resources are not compatible with the
// given checkpointed resources.
Try<Resources> applyCheckpointedResources(
    const Resources& resources,
    const Resources& checkpointedResources);

} // namespace mesos {

#endif // __RESOURCES_UTILS_HPP__
