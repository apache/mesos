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

#include <stout/hashmap.hpp>
#include <stout/multihashmap.hpp>
#include <stout/option.hpp>

#include "master/allocator.hpp"


namespace mesos {
namespace internal {
namespace master {

// Forward declarations.
class Filter;
class RefusedFilter;


class DominantShareAllocator : public Allocator
{
public:
  DominantShareAllocator() : initialized(false) {}

  virtual ~DominantShareAllocator() {}

  virtual void initialize(const process::PID<Master>& master);

  virtual void frameworkAdded(const FrameworkID& frameworkId,
                              const FrameworkInfo& frameworkInfo,
                              const Resources& used);

  virtual void frameworkDeactivated(const FrameworkID& frameworkId);

  virtual void frameworkRemoved(const FrameworkID& frameworkId);

  virtual void slaveAdded(const SlaveID& slaveId,
                          const SlaveInfo& slaveInfo,
                          const hashmap<FrameworkID, Resources>& used);

  virtual void slaveRemoved(const SlaveID& slaveId);

  virtual void updateWhitelist(
      const Option<hashset<std::string> >& whitelist);

  virtual void resourcesRequested(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests);

  virtual void resourcesUnused(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources,
    const Option<Filters>& filters);

  virtual void resourcesRecovered(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources);

  virtual void offersRevived(const FrameworkID& frameworkId);

private:
  // Allocate any allocatable resources.
  void allocate();

  // Allocate resources just from the specified slave.
  void allocate(const SlaveID& slaveId);

  // Allocate resources from the specified slaves.
  void allocate(const hashset<SlaveID>& slaveIds);

  // Remove a filter for the specified framework.
  void expire(const FrameworkID& frameworkId, Filter* filter);

  // Checks whether the slave is whitelisted.
  bool isWhitelisted(const SlaveID& slave);

  bool initialized;

  process::PID<Master> master;

  // Frameworks that are currently active.
  hashmap<FrameworkID, FrameworkInfo> frameworks;

  // Slaves that are currently active.
  hashmap<SlaveID, SlaveInfo> slaves;

  // Resources in the cluster.
  Resources resources;

  // Resources allocated to frameworks.
  hashmap<FrameworkID, Resources> allocated;

  // Resources that are allocatable on a slave.
  hashmap<SlaveID, Resources> allocatable;

  // Filters that have been added by frameworks.
  multihashmap<FrameworkID, Filter*> filters;

  // Slaves to send offers for.
  Option<hashset<std::string> > whitelist;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __DOMINANT_SHARE_ALLOCATOR_HPP__
