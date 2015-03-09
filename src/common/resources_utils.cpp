/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/stringify.hpp>

#include "common/resources_utils.hpp"

namespace mesos {

bool needCheckpointing(const Resource& resource)
{
  // TODO(mpark): Consider framework reservations.
  return Resources::isPersistentVolume(resource);
}


Try<Resources> applyCheckpointedResources(
    const Resources& resources,
    const Resources& checkpointedResources)
{
  Resources totalResources = resources;

  foreach (const Resource& resource, checkpointedResources) {
    if (!needCheckpointing(resource)) {
      return Error("Unexpected checkpointed resources " + stringify(resource));
    }

    // TODO(jieyu): Apply RESERVE operation if 'resource' is
    // dynamically reserved.

    if (Resources::isPersistentVolume(resource)) {
      Offer::Operation create;
      create.set_type(Offer::Operation::CREATE);
      create.mutable_create()->add_volumes()->CopyFrom(resource);

      Try<Resources> applied = totalResources.apply(create);
      if (applied.isError()) {
        return Error(
            "Cannot find transition for checkpointed resource " +
            stringify(resource));
      }

      totalResources = applied.get();
    }
  }

  return totalResources;
}

} // namespace mesos {
