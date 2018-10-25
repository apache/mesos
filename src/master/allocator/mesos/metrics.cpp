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

#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/push_gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/hashmap.hpp>

#include "common/protobuf_utils.hpp"

#include "master/metrics.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

using std::string;

using process::metrics::PullGauge;
using process::metrics::PushGauge;

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
    allocation_run("allocator/mesos/allocation_run", Hours(1)),
    allocation_run_latency("allocator/mesos/allocation_run_latency", Hours(1))
{
  process::metrics::add(event_queue_dispatches);
  process::metrics::add(event_queue_dispatches_);
  process::metrics::add(allocation_runs);
  process::metrics::add(allocation_run);
  process::metrics::add(allocation_run_latency);

  // Create and install gauges for the total and allocated
  // amount of standard scalar resources.
  //
  // TODO(bbannier) Add support for more than just scalar resources.
  // TODO(bbannier) Simplify this once MESOS-3214 is fixed.
  // TODO(dhamon): Set these up dynamically when adding a slave based on the
  // resources the slave exposes.
  string resources[] = {"cpus", "mem", "disk"};

  foreach (const string& resource, resources) {
    PullGauge total(
        "allocator/mesos/resources/" + resource + "/total",
        defer(allocator,
              &HierarchicalAllocatorProcess::_resources_total,
              resource));

    PullGauge offered_or_allocated(
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
  process::metrics::remove(allocation_run_latency);

  foreach (const PullGauge& gauge, resources_total) {
    process::metrics::remove(gauge);
  }

  foreach (const PullGauge& gauge, resources_offered_or_allocated) {
    process::metrics::remove(gauge);
  }

  foreachkey (const string& role, quota_allocated) {
    foreachvalue (const PullGauge& gauge, quota_allocated[role]) {
      process::metrics::remove(gauge);
    }
  }

  foreachkey (const string& role, quota_guarantee) {
    foreachvalue (const PullGauge& gauge, quota_guarantee[role]) {
      process::metrics::remove(gauge);
    }
  }

  foreachvalue (const PullGauge& gauge, offer_filters_active) {
    process::metrics::remove(gauge);
  }
}


void Metrics::setQuota(const string& role, const Quota& quota)
{
  CHECK(!quota_allocated.contains(role));

  hashmap<string, PullGauge> allocated;
  hashmap<string, PullGauge> guarantees;

  foreach (const Resource& resource, quota.info.guarantee()) {
    CHECK_EQ(Value::SCALAR, resource.type());
    double value = resource.scalar().value();

    PullGauge guarantee = PullGauge(
        "allocator/mesos/quota"
        "/roles/" + role +
        "/resources/" + resource.name() +
        "/guarantee",
        process::defer([value]() { return value; }));

    PullGauge offered_or_allocated(
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
  CHECK(quota_guarantee.contains(role));

  foreachvalue (const PullGauge& gauge, quota_allocated[role]) {
    process::metrics::remove(gauge);
  }

  foreachvalue (const PullGauge& gauge, quota_guarantee[role]) {
    process::metrics::remove(gauge);
  }

  quota_allocated.erase(role);
  quota_guarantee.erase(role);
}


void Metrics::addRole(const string& role)
{
  CHECK(!offer_filters_active.contains(role));

  PullGauge gauge(
      "allocator/mesos/offer_filters/roles/" + role + "/active",
      defer(allocator,
            &HierarchicalAllocatorProcess::_offer_filters_active,
            role));

  offer_filters_active.put(role, gauge);

  process::metrics::add(gauge);
}


void Metrics::removeRole(const string& role)
{
  Option<PullGauge> gauge = offer_filters_active.get(role);

  CHECK_SOME(gauge);

  offer_filters_active.erase(role);

  process::metrics::remove(gauge.get());
}


FrameworkMetrics::FrameworkMetrics(
    const FrameworkInfo& _frameworkInfo,
    const bool _publishPerFrameworkMetrics)
  : frameworkInfo(_frameworkInfo),
    publishPerFrameworkMetrics(_publishPerFrameworkMetrics)
{
  // TODO(greggomann): Calling `getRoles` below copies the roles from the
  // framework info, which could become expensive if the number of roles grows
  // large. Consider optimizing this.
  foreach (
      const string& role,
      protobuf::framework::getRoles(frameworkInfo)) {
    addSubscribedRole(role);
  }
}


FrameworkMetrics::~FrameworkMetrics()
{
  foreach (const string& role, suppressed.keys()) {
    removeSubscribedRole(role);
  }

  CHECK(suppressed.empty());
}


void FrameworkMetrics::reviveRole(const string& role)
{
  auto iter = suppressed.find(role);
  CHECK(iter != suppressed.end());
  iter->second = 0;
}


void FrameworkMetrics::suppressRole(const string& role)
{
  auto iter = suppressed.find(role);
  CHECK(iter != suppressed.end());
  iter->second = 1;
}


void FrameworkMetrics::addSubscribedRole(const string& role)
{
  auto result = suppressed.emplace(
      role,
      PushGauge(
          getFrameworkMetricPrefix(frameworkInfo) + "roles/" +
          role + "/suppressed"));

  CHECK(result.second);
  addMetric(result.first->second);
}


void FrameworkMetrics::removeSubscribedRole(const string& role)
{
  auto iter = suppressed.find(role);

  CHECK(iter != suppressed.end());
  removeMetric(iter->second);
  suppressed.erase(iter);
}


template <typename T>
void FrameworkMetrics::addMetric(const T& metric) {
  if (publishPerFrameworkMetrics) {
    process::metrics::add(metric);
  }
}


template <typename T>
void FrameworkMetrics::removeMetric(const T& metric) {
  if (publishPerFrameworkMetrics) {
    process::metrics::remove(metric);
  }
}


} // namespace internal {
} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {
