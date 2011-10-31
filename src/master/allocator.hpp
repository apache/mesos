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

#ifndef __ALLOCATOR_HPP__
#define __ALLOCATOR_HPP__

#include "common/hashmap.hpp"
#include "common/resources.hpp"

#include "master/master.hpp"


namespace mesos {
namespace internal {
namespace master {

// Basic model of an allocator: resources are allocated to a framework
// in the form of offers. A framework can refuse some resources in
// offers and run tasks in others. Resources can be recovered from a
// framework when tasks finish/fail (or are lost due to a slave
// failure) or when an offer is rescinded.

class Allocator {
public:
  virtual ~Allocator() {}

  virtual void initialize(Master* _master) {}

  virtual void frameworkAdded(Framework* framework) {}

  virtual void frameworkRemoved(Framework* framework) {}

  virtual void slaveAdded(Slave* slave) {}

  virtual void slaveRemoved(Slave* slave) {}

  virtual void resourcesRequested(
      const FrameworkID& frameworkId,
      const std::vector<ResourceRequest>& requests) {}

  // Whenever resources offered to a framework go unused (e.g.,
  // refused) the master invokes this callback.
  virtual void resourcesUnused(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources) {}

  // Whenever resources are "recovered" in the cluster (e.g., a task
  // finishes, an offer is removed because a framework has failed or
  // is failing over) the master invokes this callback.
  virtual void resourcesRecovered(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources) {}

  // Whenever a framework that has filtered resources want's to revive
  // offers for those resources the master invokes this callback.
  virtual void offersRevived(Framework* framework) {}

  virtual void timerTick() {}
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __ALLOCATOR_HPP__
