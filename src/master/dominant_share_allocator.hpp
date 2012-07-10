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

#ifndef __DOMINANT_SHARE_ALLOCATOR_HPP__
#define __DOMINANT_SHARE_ALLOCATOR_HPP__

#include <vector>

#include "common/hashmap.hpp"
#include "common/multihashmap.hpp"

#include "master/allocator.hpp"


namespace mesos {
namespace internal {
namespace master {

class DominantShareAllocator : public Allocator
{
public:
  DominantShareAllocator(): initialized(false) {}

  virtual ~DominantShareAllocator() {}

  virtual void initialize(Master* _master);

  virtual void frameworkAdded(Framework* framework);

  virtual void frameworkRemoved(Framework* framework);

  virtual void slaveAdded(Slave* slave);

  virtual void slaveRemoved(Slave* slave);

  virtual void resourcesRequested(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests);

  virtual void resourcesUnused(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources);

  virtual void resourcesRecovered(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources);

  virtual void offersRevived(Framework* framework);

  virtual void timerTick();

private:
  // Get an ordering to consider frameworks in for launching tasks.
  std::vector<Framework*> getAllocationOrdering();

  // Look at the full state of the cluster and send out offers.
  void makeNewOffers();

  // Make resource offers for just one slave.
  void makeNewOffers(Slave* slave);

  // Make resource offers for a subset of the slaves.
  void makeNewOffers(const std::vector<Slave*>& slaves);

  bool initialized;

  Master* master;

  Resources totalResources;

  // Remember which frameworks refused each slave "recently"; this is
  // cleared when the slave's free resources go up or when everyone
  // has refused it.
  multihashmap<SlaveID, FrameworkID> refusers;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __DOMINANT_SHARE_ALLOCATOR_HPP__
