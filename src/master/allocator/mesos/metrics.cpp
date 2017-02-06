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

#include "master/allocator/mesos/metrics.hpp"

#include <string>

#include <mesos/quota/quota.hpp>

#include <process/metrics/gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/hashmap.hpp>

#include "master/allocator/mesos/hierarchical.hpp"

using std::string;

using process::metrics::Gauge;

namespace mesos {
namespace internal {
namespace master {
namespace allocator {
namespace internal {

Metrics::Metrics(const HierarchicalAllocatorProcess& _allocator)
  : allocator(_allocator.self()),
    event_queue_dispatches(
        "allocator/mesos/event_queue_dispatches",
        process::defer(
            allocator, &HierarchicalAllocatorProcess::_event_queue_dispatches)),
    event_queue_dispatches_(
        "allocator/event_queue_dispatches",
        process::defer(
            allocator, &HierarchicalAllocatorProcess::_event_queue_dispatches)),
    allocation_runs("allocator/mesos/allocation_runs"),
    allocation_run("allocator/mesos/allocation_run", Hours(1))
{
  process::metrics::add(event_queue_dispatches);
  process::metrics::add(event_queue_dispatches_);
  process::metrics::add(allocation_runs);
  process::metrics::add(allocation_run);

  // Create and install gauges for the total and allocated
  // amount of standard scalar resources.
  //
  // TODO(bbannier) Add support for more than just scalar resources.
  // TODO(bbannier) Simplify this once MESOS-3214 is fixed.
  // TODO(dhamon): Set these up dynamically when adding a slave based on the
  // resources the slave exposes.
  string resources[] = {"cpus", "mem", "disk"};

  foreach (const string& resource, resources) {
    Gauge total(
        "allocator/mesos/resources/" + resource + "/total",
        defer(allocator,
              &HierarchicalAllocatorProcess::_resources_total,
              resource));

    Gauge offered_or_allocated(
        "allocator/mesos/resources/" + resource + "/offered_or_allocated",
        defer(allocator,
              &HierarchicalAllocatorProcess::_resources_offered_or_allocated,
              resource));

    resources_total.push_back(total);
    resources_offered_or_allocated.push_back(offered_or_allocated);

    process::metrics::add(total);
    process::metrics::add(offered_or_allocated);
  }
}


Metrics::~Metrics()
{
  process::metrics::remove(event_queue_dispatches);
  process::metrics::remove(event_queue_dispatches_);
  process::metrics::remove(allocation_runs);
  process::metrics::remove(allocation_run);

  foreach (const Gauge& gauge, resources_total) {
    process::metrics::remove(gauge);
  }

  foreach (const Gauge& gauge, resources_offered_or_allocated) {
    process::metrics::remove(gauge);
  }

  foreachkey (const string& role, quota_allocated) {
    foreachvalue (const Gauge& gauge, quota_allocated[role]) {
      process::metrics::remove(gauge);
    }
  }

  foreachkey (const string& role, quota_guarantee) {
    foreachvalue (const Gauge& gauge, quota_guarantee[role]) {
      process::metrics::remove(gauge);
    }
  }

  foreachvalue (const Gauge& gauge, offer_filters_active) {
    process::metrics::remove(gauge);
  }
}


void Metrics::setQuota(const string& role, const Quota& quota)
{
  CHECK(!quota_allocated.contains(role));

  hashmap<string, Gauge> allocated;
  hashmap<string, Gauge> guarantees;

  foreach (const Resource& resource, quota.info.guarantee()) {
    CHECK_EQ(Value::SCALAR, resource.type());
    double value = resource.scalar().value();

    Gauge guarantee = Gauge(
        "allocator/mesos/quota"
        "/roles/" + role +
        "/resources/" + resource.name() +
        "/guarantee",
        process::defer([value]() { return value; }));

    Gauge offered_or_allocated(
        "allocator/mesos/quota"
        "/roles/" + role +
        "/resources/" + resource.name() +
        "/offered_or_allocated",
        defer(allocator,
              &HierarchicalAllocatorProcess::_quota_allocated,
              role,
              resource.name()));

    guarantees.put(resource.name(), guarantee);
    allocated.put(resource.name(), offered_or_allocated);

    process::metrics::add(guarantee);
    process::metrics::add(offered_or_allocated);
  }

  quota_allocated[role] = allocated;
  quota_guarantee[role] = guarantees;
}


void Metrics::removeQuota(const string& role)
{
  CHECK(quota_allocated.contains(role));

  foreachvalue (const Gauge& gauge, quota_allocated[role]) {
    process::metrics::remove(gauge);
  }

  quota_allocated.erase(role);
}


void Metrics::addRole(const string& role)
{
  CHECK(!offer_filters_active.contains(role));

  Gauge gauge(
      "allocator/mesos/offer_filters/roles/" + role + "/active",
      defer(allocator,
            &HierarchicalAllocatorProcess::_offer_filters_active,
            role));

  offer_filters_active.put(role, gauge);

  process::metrics::add(gauge);
}


void Metrics::removeRole(const string& role)
{
  Option<Gauge> gauge = offer_filters_active.get(role);

  CHECK_SOME(gauge);

  offer_filters_active.erase(role);

  process::metrics::remove(gauge.get());
}

} // namespace internal {
} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {
