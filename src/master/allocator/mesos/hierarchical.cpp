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

#include "master/allocator/mesos/hierarchical.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <mesos/attributes.hpp>
#include <mesos/resource_quantities.hpp>
#include <mesos/resources.hpp>
#include <mesos/roles.hpp>
#include <mesos/type_utils.hpp>

#include <process/after.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/event.hpp>
#include <process/help.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>
#include <process/timeout.hpp>

#include <stout/check.hpp>
#include <stout/hashset.hpp>
#include <stout/set.hpp>
#include <stout/stopwatch.hpp>
#include <stout/stringify.hpp>

#include "common/authorization.hpp"
#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"

using std::make_shared;
using std::set;
using std::shared_ptr;
using std::string;
using std::vector;
using std::weak_ptr;

using mesos::allocator::FrameworkOptions;
using mesos::allocator::InverseOfferStatus;
using mesos::allocator::Options;

using process::after;
using process::http::authentication::Principal;
using process::Continue;
using process::ControlFlow;
using process::Failure;
using process::Future;
using process::loop;
using process::Owned;
using process::PID;
using process::Timeout;


namespace mesos {

// Needed to prevent shadowing of template '::operator-<std::set<T>>'
// by non-template '::mesos::operator-'
using ::operator-;

namespace internal {
namespace master {
namespace allocator {
namespace internal {

// Used to represent "filters" for resources unused in offers.
class OfferFilter
{
public:
  virtual ~OfferFilter() {}

  virtual bool filter(const Resources& resources) const = 0;
};


class RefusedOfferFilter : public OfferFilter
{
public:
  RefusedOfferFilter(
      const Resources& _resources,
      const Duration& timeout)
    : _resources(_resources),
      _expired(after(timeout)) {}

  ~RefusedOfferFilter() override
  {
    // Cancel the timeout upon destruction to avoid lingering timers.
    _expired.discard();
  }

  Future<Nothing> expired() const { return _expired; };

  bool filter(const Resources& resources) const override
  {
    // NOTE: We do not check for the filter being expired here
    // because `recoverResources()` expects the filter to apply
    // until the filter is removed, see:
    // https://github.com/apache/mesos/commit/2f170f302fe94c4
    //
    // TODO(jieyu): Consider separating the superset check for regular
    // and revocable resources. For example, frameworks might want
    // more revocable resources only or non-revocable resources only,
    // but currently the filter only expires if there is more of both
    // revocable and non-revocable resources.
    return _resources.contains(resources); // Refused resources are superset.
  }

private:
  const Resources _resources;
  Future<Nothing> _expired;
};


// Used to represent "filters" for inverse offers.
//
// NOTE: Since this specific allocator implementation only sends inverse offers
// for maintenance primitives, and those are at the whole slave level, we only
// need to filter based on the time-out.
// If this allocator implementation starts sending out more resource specific
// inverse offers, then we can capture the `unavailableResources` in the filter
// function.
class InverseOfferFilter
{
public:
  virtual ~InverseOfferFilter() {}

  virtual bool filter() const = 0;
};


// NOTE: See comment above `InverseOfferFilter` regarding capturing
// `unavailableResources` if this allocator starts sending fine-grained inverse
// offers.
class RefusedInverseOfferFilter : public InverseOfferFilter
{
public:
  RefusedInverseOfferFilter(const Duration& timeout)
    : _expired(after(timeout)) {}

  ~RefusedInverseOfferFilter() override
  {
    // Cancel the timeout upon destruction to avoid lingering timers.
    _expired.discard();
  }

  Future<Nothing> expired() const { return _expired; };

  bool filter() const override
  {
    // See comment above why we currently don't do more fine-grained filtering.
    return _expired.isPending();
  }

private:
  Future<Nothing> _expired;
};


// Helper function to unpack a map of per-role `OfferFilters` to the
// format used by the allocator.
static hashmap<string, vector<ResourceQuantities>> unpackFrameworkOfferFilters(
    const ::google::protobuf::Map<string, OfferFilters>& roleOfferFilters)
{
  hashmap<string, vector<ResourceQuantities>> result;

  // Use `auto` in place of `protobuf::MapPair<string, AllocatableResources>`
  // below since `foreach` is a macro and cannot contain angle brackets.
  foreach (auto&& offerFilters, roleOfferFilters) {
    const string& role = offerFilters.first;
    const OfferFilters& allocatableResources = offerFilters.second;

    if (allocatableResources.has_min_allocatable_resources()) {
      result.insert({role, {}});

      vector<ResourceQuantities>& allocatableResourcesRole = result[role];

      foreach (
          const OfferFilters::ResourceQuantities& quantities,
          allocatableResources.min_allocatable_resources().quantities()) {
        allocatableResourcesRole.push_back(
          ResourceQuantities(quantities.quantities()));
      }
    }
  }

  return result;
}


void ScalarResourceTotals::add(
    const SlaveID& slaveID,
    const Resources& resources)
{
  if (resources.scalars().empty()) {
    // In this case, we avoid adding an entry to `scalars` to maintain the
    // invariant that `scalars` doesn't track agents with empty resources.
    return;
  }

  scalarsTotal -= ResourceQuantities::fromScalarResources(scalars[slaveID]);
  scalars.at(slaveID) += resources.scalars();
  scalarsTotal += ResourceQuantities::fromScalarResources(scalars.at(slaveID));
}


void ScalarResourceTotals::subtract(
    const SlaveID& slaveID,
    const Resources& resources)
{
  if (resources.scalars().empty()) {
    // `scalars` does not track agents with empty resources, thus subtracting
    // empty resources from an agent is valid regardless of whether its
    // resources are tracked in `scalars`.
    return;
  }

  CHECK_CONTAINS(scalars, slaveID);
  CHECK_CONTAINS(scalars.at(slaveID), resources.scalars());

  scalarsTotal -= ResourceQuantities::fromScalarResources(scalars.at(slaveID));
  scalars.at(slaveID) -= resources.scalars();
  scalarsTotal += ResourceQuantities::fromScalarResources(scalars.at(slaveID));

  if (scalars.at(slaveID).empty()) {
    scalars.erase(slaveID);
  }
}


Role::Role(const string& _role, Role* _parent)
  : role(_role),
    basename(strings::split(role, "/").back()),
    parent(_parent),
    quota_(DEFAULT_QUOTA),
    weight_(DEFAULT_WEIGHT) {}


void Role::addChild(Role* child)
{
  CHECK_NOT_CONTAINS(children_, child->basename);
  children_.put(child->basename, child);
}


void Role::removeChild(Role* child)
{
  CHECK_CONTAINS(children_, child->basename);
  children_.erase(child->basename);
}


RoleTree::RoleTree() : root_(new Role("", nullptr)) {}


RoleTree::RoleTree(Metrics* metrics_)
  : root_(new Role("", nullptr)), metrics(metrics_) {}


RoleTree::~RoleTree()
{
  delete root_;
}


Option<const Role*> RoleTree::get(const std::string& role) const
{
  auto found = roles_.find(role);

  if (found == roles_.end()) {
    return None();
  } else {
    return &(found->second);
  }
}


Option<Role*> RoleTree::get_(const std::string& role)
{
  auto found = roles_.find(role);

  if (found == roles_.end()) {
    return None();
  } else {
    return &(found->second);
  }
}


Role& RoleTree::operator[](const std::string& rolePath)
{
  if (roles_.contains(rolePath)) {
    return roles_.at(rolePath);
  }

  // We go through the path from top to bottom and create any missing
  // node along the way.
  Role* current = root_;
  foreach (const string& token, strings::split(rolePath, "/")) {
    Option<Role*> child = current->children_.get(token);

    if (child.isSome()) {
      current = *child;
      continue;
    }

    // Create a new role.
    string newRolePath =
      current == root_ ? token : strings::join("/", current->role, token);
    CHECK_NOT_CONTAINS(roles_, newRolePath);
    roles_.put(newRolePath, Role(newRolePath, current));

    if (metrics.isSome()) {
      (*metrics)->addRole(newRolePath);
    }

    Role& role = roles_.at(newRolePath);
    current->addChild(&role);
    current = &role;
  }

  return roles_.at(rolePath);
}


bool RoleTree::tryRemove(const std::string& role)
{
  CHECK_CONTAINS(roles_, role);
  Role* current = &(roles_.at(role));

  if (!current->isEmpty()) {
    return false;
  }

  // We go through the path from bottom to top and remove empty nodes
  // along the way.
  vector<string> tokens = strings::split(role, "/");
  for (auto token = tokens.crbegin(); token != tokens.crend(); ++token) {
    CHECK_EQ(current->basename, *token);
    if (!current->isEmpty()) {
      break;
    }

    CHECK(current->allocatedUnreservedNonRevocable.empty())
      << "An empty role " << current->role
      << " has non-empty allocated scalar resources: "
      << current->allocatedUnreservedNonRevocable.quantities();

    Role* parent = CHECK_NOTNULL(current->parent);

    parent->removeChild(current);

    if (metrics.isSome()) {
      (*metrics)->removeRole(current->role);
    }

    CHECK(current->offeredOrAllocatedUnreservedNonRevocable.empty())
      << "An empty role " << current->role
      << " has non-empty offered or allocated"
      << " unreserved non-revocable scalar resources: "
      << current->offeredOrAllocatedUnreservedNonRevocable.quantities();

    CHECK(current->offeredOrAllocatedReserved.empty())
      << "An empty role " << current->role
      << " has non-empty offered or allocated reserved scalar resources: "
      << current->offeredOrAllocatedReserved.quantities();

    roles_.erase(current->role);

    current = parent;
  }

  return true;
}


void RoleTree::updateQuotaConsumedMetric(const Role* role)
{
  if (metrics.isSome()) {
    (*metrics)->updateConsumed(role->role, role->quotaConsumed());
  }
}


void RoleTree::trackReservations(const Resources& resources)
{
  foreach (const Resource& r, resources.scalars()) {
    CHECK(Resources::isReserved(r));

    const string& reservationRole = Resources::reservationRole(r);
    ResourceQuantities quantities = ResourceQuantities::fromScalarResources(r);

    // NOTE: If necessary, a new role tree node is created.
    applyToRoleAndAncestors(&(*this)[reservationRole], [&](Role* current) {
      current->reservationScalarQuantities_ += quantities;
      updateQuotaConsumedMetric(current);
    });
  }
}


void RoleTree::untrackReservations(const Resources& resources)
{
  foreach (const Resource& r, resources.scalars()) {
    CHECK(Resources::isReserved(r));

    const string& reservationRole = Resources::reservationRole(r);
    ResourceQuantities quantities = ResourceQuantities::fromScalarResources(r);

    applyToRoleAndAncestors(
      CHECK_NOTNONE(get_(reservationRole)), [&](Role* current) {
        CHECK_CONTAINS(current->reservationScalarQuantities_, quantities);
        current->reservationScalarQuantities_ -= quantities;
        updateQuotaConsumedMetric(current);
      });

    tryRemove(reservationRole);
  }
}


void RoleTree::trackAllocated(
    const SlaveID& slaveId,
    const Resources& resources_)
{
  foreachpair (
      const string& role,
      const Resources& resources,
      resources_.scalars().unreserved().nonRevocable().allocations()) {
    applyToRoleAndAncestors(CHECK_NOTNONE(get_(role)), [&](Role* current) {
      current->allocatedUnreservedNonRevocable.add(slaveId, resources);
      updateQuotaConsumedMetric(current);
    });
  }
}


void RoleTree::untrackAllocated(
    const SlaveID& slaveId,
    const Resources& resources_)
{
  foreachpair (
      const string& role,
      const Resources& resources,
      resources_.scalars().unreserved().nonRevocable().allocations()) {
    applyToRoleAndAncestors(CHECK_NOTNONE(get_(role)), [&](Role* current) {
      current->allocatedUnreservedNonRevocable.subtract(slaveId, resources);
      updateQuotaConsumedMetric(current);
    });
  }
}


void RoleTree::trackFramework(
    const FrameworkID& frameworkId, const string& rolePath)
{
  Role* role = &(*this)[rolePath];

  CHECK_NOT_CONTAINS(role->frameworks_, frameworkId)
    << " for role " << rolePath;
  role->frameworks_.insert(frameworkId);
}


void RoleTree::untrackFramework(
    const FrameworkID& frameworkId, const string& rolePath)
{
  CHECK_CONTAINS(roles_, rolePath);
  Role& role = roles_.at(rolePath);

  CHECK_CONTAINS(role.frameworks_, frameworkId) << " for role " << rolePath;
  role.frameworks_.erase(frameworkId);

  tryRemove(rolePath);
}


void RoleTree::updateQuota(const string& role, const Quota& quota)
{
  (*this)[role].quota_ = quota;

  tryRemove(role);
}


void RoleTree::updateWeight(const string& role, double weight)
{
  (*this)[role].weight_ = weight;

  tryRemove(role);
}


void RoleTree::trackOfferedOrAllocated(
    const SlaveID& slaveId,
    const Resources& resources_)
{
  // TODO(mzhu): avoid building a map by traversing `resources`
  // and look for the allocation role of individual resource.
  // However, due to MESOS-9242, this currently does not work
  // as traversing resources would lose the shared count.
  foreachpair (
      const string& role,
      const Resources& resources,
      resources_.scalars().allocations()) {
    applyToRoleAndAncestors(
        CHECK_NOTNONE(get_(role)), [&resources, &slaveId](Role* current) {
          current->offeredOrAllocatedReserved.add(
              slaveId, resources.reserved());

          current->offeredOrAllocatedUnreservedNonRevocable.add(
              slaveId, resources.unreserved().nonRevocable());
        });
  }
}


void RoleTree::untrackOfferedOrAllocated(
    const SlaveID& slaveId,
    const Resources& resources_)
{
  // TODO(mzhu): avoid building a map by traversing `resources`
  // and look for the allocation role of individual resource.
  // However, due to MESOS-9242, this currently does not work
  // as traversing resources would lose the shared count.
  foreachpair (
      const string& role,
      const Resources& resources,
      resources_.scalars().allocations()) {
    applyToRoleAndAncestors(
        CHECK_NOTNONE(get_(role)), [&resources, &slaveId](Role* current) {
          current->offeredOrAllocatedReserved.subtract(
              slaveId, resources.reserved());

          current->offeredOrAllocatedUnreservedNonRevocable.subtract(
              slaveId, resources.unreserved().nonRevocable());
        });
  }
}


std::string RoleTree::toJSON() const
{
  std::function<void(JSON::ObjectWriter*, const Role*)> json =
    [&](JSON::ObjectWriter* writer, const Role* role) {
      writer->field("basename", role->basename);
      writer->field("role", role->role);
      writer->field("weight", role->weight_);
      writer->field("guarantees", role->quota_.guarantees);
      writer->field("limits", role->quota_.limits);
      writer->field(
          "reservation_quantities", role->reservationScalarQuantities_);

      writer->field(
          "offered_or_allocated_reserved_quantities",
          role->offeredOrAllocatedReserved.quantities());

      writer->field(
          "offered_or_allocated_unreserved_nonrevocable_quantities",
          role->offeredOrAllocatedUnreservedNonRevocable.quantities());

      writer->field("frameworks", [&](JSON::ArrayWriter* writer) {
        foreach (const FrameworkID& id, role->frameworks_) {
          writer->element(id.value());
        }
      });

      writer->field("children", [&](JSON::ArrayWriter* writer) {
        foreachvalue (const Role* child, role->children_) {
          writer->element(
              [&](JSON::ObjectWriter* writer) { json(writer, child); });
        }
      });
    };

  auto tree = [&](JSON::ObjectWriter* writer) { json(writer, root_); };

  return jsonify(tree);
}


Framework::Framework(
    const FrameworkInfo& frameworkInfo,
    FrameworkOptions&& options,
    bool _active,
    bool publishPerFrameworkMetrics)
  : frameworkId(frameworkInfo.id()),
    info(frameworkInfo),
    roles(protobuf::framework::getRoles(frameworkInfo)),
    suppressedRoles(std::move(options.suppressedRoles)),
    capabilities(frameworkInfo.capabilities()),
    active(_active),
    metrics(new FrameworkMetrics(frameworkInfo, publishPerFrameworkMetrics)),
    minAllocatableResources(
        unpackFrameworkOfferFilters(frameworkInfo.offer_filters())),
    offerConstraintsFilter(std::move(options.offerConstraintsFilter))
{}


static string OFFER_CONSTRAINTS_DEBUG_HELP();


void HierarchicalAllocatorProcess::initialize(
    const Options& _options,
    const lambda::function<
        void(const FrameworkID&,
             const hashmap<string, hashmap<SlaveID, Resources>>&)>&
      _offerCallback,
    const lambda::function<
        void(const FrameworkID&,
             const hashmap<SlaveID, UnavailableResources>&)>&
      _inverseOfferCallback)
{
  options = _options;
  offerCallback = _offerCallback;
  inverseOfferCallback = _inverseOfferCallback;
  initialized = true;
  paused = false;

  completedFrameworkMetrics =
    BoundedHashMap<FrameworkID, process::Owned<FrameworkMetrics>>(
        options.maxCompletedFrameworks);

  route("/offer_constraints_debug",
        options.readonlyHttpAuthenticationRealm.getOrElse(""),
        OFFER_CONSTRAINTS_DEBUG_HELP(),
        [this](const process::http::Request& request,
               const Option<Principal>& principal) {
          logRequest(request);
          return offerConstraintsDebug(request, principal)
            .onReady([request](const process::http::Response& response) {
              logResponse(request, response);
            });
        });

  roleSorter->initialize(options.fairnessExcludeResourceNames);

  VLOG(1) << "Initialized hierarchical allocator process";

  // Start a loop to run allocation periodically.
  PID<HierarchicalAllocatorProcess> _self = self();

  // Set a temporary variable for the lambda capture.
  Duration allocationInterval = options.allocationInterval;
  loop(
      None(), // Use `None` so we iterate outside the allocator process.
      [allocationInterval]() {
        return after(allocationInterval);
      },
      [_self](const Nothing&) {
        return dispatch(_self, &HierarchicalAllocatorProcess::generateOffers)
          .then([]() -> ControlFlow<Nothing> { return Continue(); });
      });
}


void HierarchicalAllocatorProcess::recover(
    const int _expectedAgentCount,
    const hashmap<string, Quota>& quotas)
{
  // Recovery should start before actual allocation starts.
  CHECK(initialized);
  CHECK(slaves.empty());
  CHECK(_expectedAgentCount >= 0);

  // If there is no quota, recovery is a no-op. Otherwise, we need
  // to delay allocations while agents are reregistering because
  // otherwise we perform allocations on a partial view of resources!
  // We would consequently perform unnecessary allocations to satisfy
  // quota constraints, which can over-allocate non-revocable resources
  // to roles using quota. Then, frameworks in roles without quota can
  // be unnecessarily deprived of resources. We may also be unable to
  // satisfy all of the quota constraints. Repeated master failovers
  // exacerbate the issue.

  if (quotas.empty()) {
    VLOG(1) << "Skipping recovery of hierarchical allocator:"
            << " nothing to recover";

    return;
  }

  foreachpair (const string& role, const Quota& quota, quotas) {
    updateQuota(role, quota);
  }

  // Record the number of expected agents.
  expectedAgentCount =
    static_cast<int>(_expectedAgentCount * options.agentRecoveryFactor);

  // Skip recovery if there are no expected agents. This is not strictly
  // necessary for the allocator to function correctly, but maps better
  // to expected behavior by the user: the allocator is not paused until
  // a new agent is added.
  if (expectedAgentCount.get() == 0) {
    VLOG(1) << "Skipping recovery of hierarchical allocator:"
            << " no reconnecting agents to wait for";

    return;
  }

  // Pause allocation until after a sufficient amount of agents reregister
  // or a timer expires.
  pause();

  // Setup recovery timer.
  delay(options.recoveryTimeout, self(), &Self::resume);

  LOG(INFO) << "Triggered allocator recovery: waiting for "
            << expectedAgentCount.get() << " agents to reconnect or "
            << options.recoveryTimeout << " to pass";
}


void HierarchicalAllocatorProcess::addFramework(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const hashmap<SlaveID, Resources>& used,
    bool active,
    FrameworkOptions&& frameworkOptions)
{
  CHECK(initialized);
  CHECK_NOT_CONTAINS(frameworks, frameworkId);

  // TODO(mzhu): remove the `frameworkId` parameter.
  CHECK_EQ(frameworkId, frameworkInfo.id());

  // NOTE: We don't use insert({frameworkId, Framework()}) here because with
  // older versions of libstdc++ the argument type of
  // std::unordered_map::insert(P&&)is deduced to be a non-copyable non-movable
  // std::pair<const FrameworkId, Framework>.
  frameworks.emplace(frameworkId,
                     Framework(
                         frameworkInfo,
                         std::move(frameworkOptions),
                         active,
                         options.publishPerFrameworkMetrics));

  const Framework& framework = *CHECK_NOTNONE(getFramework(frameworkId));

  foreach (const string& role, framework.roles) {
    trackFrameworkUnderRole(framework, role);

    Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

    if (framework.suppressedRoles.count(role)) {
      frameworkSorter->deactivate(frameworkId.value());
      framework.metrics->suppressRole(role);
    } else {
      frameworkSorter->activate(frameworkId.value());
      framework.metrics->reviveRole(role);
    }
  }

  // Update the allocation for this framework.
  foreachpair (const SlaveID& slaveId, const Resources& resources, used) {
    // TODO(bmahler): The master won't tell us about resources
    // allocated to agents that have not yet been added, consider
    // CHECKing this case.
    if (!slaves.contains(slaveId)) {
      continue;
    }

    // The slave struct will already be aware of the allocated
    // resources, so we only need to track them in the sorters.
    trackAllocatedResources(slaveId, frameworkId, resources);

    roleTree.trackAllocated(slaveId, resources);
  }

  LOG(INFO) << "Added framework " << frameworkId;

  if (active) {
    generateOffers();
  } else {
    deactivateFramework(frameworkId);
  }
}


void HierarchicalAllocatorProcess::removeFramework(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);

  // To free up offered or allocated resources of a framework, we need to
  // do two things: update available resources in the agent and update
  // tracking info in the role tree and role sorter.
  // We do both at the same time.
  foreachvalue (Slave& slave, slaves) {
    const hashmap<FrameworkID, Resources>& offeredOrAllocated =
      slave.getOfferedOrAllocated();
    auto frameworkResources = offeredOrAllocated.find(frameworkId);

    if (frameworkResources == offeredOrAllocated.end()) {
      continue;
    }

    VLOG(1) << "Recovering " << frameworkResources->second
            << " from removing framework " << frameworkId
            << " (agent total: " << slave.getTotal() << ","
            << " offered or allocated: "
            << slave.getTotalOfferedOrAllocated() << ")";

    untrackAllocatedResources(
        slave.id, frameworkId, frameworkResources->second);

    // Note: this method might mutate `offeredOrAllocated`.
    slave.increaseAvailable(frameworkId, frameworkResources->second);
  }

  Framework& framework = *CHECK_NOTNONE(getFramework(frameworkId));

  // Untack framework from roles.
  foreach (const string& role, framework.roles) {
    CHECK(tryUntrackFrameworkUnderRole(framework, role))
      << " Framework: " << frameworkId << " role: " << role;
  }

  // Transfer ownership of this framework's metrics to
  // `completedFrameworkMetrics`.
  completedFrameworkMetrics.set(
      frameworkId,
      Owned<FrameworkMetrics>(framework.metrics.release()));

  frameworks.erase(frameworkId);

  LOG(INFO) << "Removed framework " << frameworkId;
}


void HierarchicalAllocatorProcess::activateFramework(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);

  Framework& framework = *CHECK_NOTNONE(getFramework(frameworkId));

  framework.active = true;

  // Activate all roles for this framework except the roles that
  // are marked as deactivated.
  // Note: A subset of framework roles can be deactivated if the
  // role is specified in `suppressed_roles` during framework
  // (re)registration, or via a subsequent `SUPPRESS` call.
  foreach (const string& role, framework.roles) {
    Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

    if (!framework.suppressedRoles.count(role)) {
      frameworkSorter->activate(frameworkId.value());
    }
  }

  LOG(INFO) << "Activated framework " << frameworkId;

  generateOffers();
}


void HierarchicalAllocatorProcess::deactivateFramework(
    const FrameworkID& frameworkId)
{
  CHECK(initialized);

  Framework& framework = *CHECK_NOTNONE(getFramework(frameworkId));

  foreach (const string& role, framework.roles) {
    Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

    frameworkSorter->deactivate(frameworkId.value());

    // Note that the Sorter *does not* remove the resources allocated
    // to this framework. For now, this is important because if the
    // framework fails over and is activated, we still want a record
    // of the resources that it is using. We might be able to collapse
    // the added/removed and activated/deactivated in the future.
  }

  framework.active = false;

  framework.offerFilters.clear();
  framework.inverseOfferFilters.clear();

  LOG(INFO) << "Deactivated framework " << frameworkId;
}


void HierarchicalAllocatorProcess::updateFramework(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    FrameworkOptions&& frameworkOptions)
{
  CHECK(initialized);

  Framework& framework = *CHECK_NOTNONE(getFramework(frameworkId));

  const set<string> oldRoles = framework.roles;
  const set<string> newRoles = protobuf::framework::getRoles(frameworkInfo);
  const set<string> oldSuppressedRoles = framework.suppressedRoles;

  foreach (const string& role, newRoles - oldRoles) {
    framework.metrics->addSubscribedRole(role);

    // NOTE: It's possible that we're already tracking this framework
    // under the role because a framework can unsubscribe from a role
    // while it still has resources allocated to the role.
    if (!isFrameworkTrackedUnderRole(frameworkId, role)) {
      // TODO(mzhu): `CHECK` the above case.
      trackFrameworkUnderRole(framework, role);
    }

    frameworkSorters.at(role)->activate(frameworkId.value());
  }

  foreach (const string& role, oldRoles - newRoles) {
    Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

    frameworkSorter->deactivate(frameworkId.value());

    tryUntrackFrameworkUnderRole(framework, role);

    if (framework.offerFilters.contains(role)) {
      framework.offerFilters.erase(role);
    }

    framework.metrics->removeSubscribedRole(role);
    framework.suppressedRoles.erase(role);
  }

  framework.info = frameworkInfo;
  framework.roles = newRoles;
  framework.capabilities =
    protobuf::framework::Capabilities(frameworkInfo.capabilities());

  framework.minAllocatableResources =
    unpackFrameworkOfferFilters(frameworkInfo.offer_filters());

  framework.offerConstraintsFilter =
    std::move(frameworkOptions.offerConstraintsFilter);

  suppressRoles(
      framework,
      frameworkOptions.suppressedRoles - oldSuppressedRoles);

  reviveRoles(
      framework,
      (oldSuppressedRoles - frameworkOptions.suppressedRoles) & newRoles);

  CHECK(framework.suppressedRoles == frameworkOptions.suppressedRoles)
    << "After updating framework " << frameworkId
    << " its set of suppressed roles " << stringify(framework.suppressedRoles)
    << " differs from required " << stringify(frameworkOptions.suppressedRoles);
}


void HierarchicalAllocatorProcess::addSlave(
    const SlaveID& slaveId,
    const SlaveInfo& slaveInfo,
    const vector<SlaveInfo::Capability>& capabilities,
    const Option<Unavailability>& unavailability,
    const Resources& total,
    const hashmap<FrameworkID, Resources>& used)
{
  CHECK(initialized);
  CHECK_NOT_CONTAINS(slaves, slaveId);
  CHECK_EQ(slaveId, slaveInfo.id());
  CHECK(!paused || expectedAgentCount.isSome());

  slaves.insert({slaveId,
                 Slave(
                     slaveInfo,
                     protobuf::slave::Capabilities(capabilities),
                     true,
                     total,
                     used)});

  Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));

  // NOTE: We currently implement maintenance in the allocator to be able to
  // leverage state and features such as the FrameworkSorter and OfferFilter.
  if (unavailability.isSome()) {
    slave.maintenance = Slave::Maintenance(unavailability.get());
  }

  roleTree.trackReservations(total.reserved());

  const ResourceQuantities agentScalarQuantities =
    ResourceQuantities::fromScalarResources(total.scalars());

  totalScalarQuantities += agentScalarQuantities;
  roleSorter->addSlave(slaveId, agentScalarQuantities);

  foreachvalue (const Owned<Sorter>& sorter, frameworkSorters) {
    sorter->addSlave(slaveId, agentScalarQuantities);
  }

  foreachpair (const FrameworkID& frameworkId,
               const Resources& allocation,
               used) {
    // There are two cases here:
    //
    //   (1) The framework has already been added to the allocator.
    //       In this case, we track the allocation in the sorters.
    //
    //   (2) The framework has not yet been added to the allocator.
    //       The master will imminently add the framework using
    //       the `FrameworkInfo` recovered from the agent, and in
    //       the interim we do not track the resources allocated to
    //       this framework. This leaves a small window where the
    //       role sorting will under-account for the roles belonging
    //       to this framework.
    //
    // TODO(bmahler): Fix the issue outlined in (2).
    if (!frameworks.contains(frameworkId)) {
      continue;
    }

    trackAllocatedResources(slaveId, frameworkId, allocation);

    roleTree.trackAllocated(slaveId, allocation);
  }

  // If we have just a number of recovered agents, we cannot distinguish
  // between "old" agents from the registry and "new" ones joined after
  // recovery has started. Because we do not persist enough information
  // to base logical decisions on, any accounting algorithm here will be
  // crude. Hence we opted for checking whether a certain amount of cluster
  // capacity is back online, so that we are reasonably confident that we
  // will not over-commit too many resources to quota that we will not be
  // able to revoke.
  if (paused &&
      expectedAgentCount.isSome() &&
      (static_cast<int>(slaves.size()) >= expectedAgentCount.get())) {
    VLOG(1) << "Recovery complete: sufficient amount of agents added; "
            << slaves.size() << " agents known to the allocator";

    expectedAgentCount = None();
    resume();
  }

  LOG(INFO)
    << "Added agent " << slaveId << " (" << slave.info.hostname() << ")"
    << " with " << slave.getTotal()
    << " (offered or allocated: " << slave.getTotalOfferedOrAllocated() << ")";

  generateOffers(slaveId);
}


void HierarchicalAllocatorProcess::removeSlave(
    const SlaveID& slaveId)
{
  CHECK(initialized);

  {
    const Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));

    // untrackAllocatedResources() potentially removes allocation roles, thus
    // we need to untrack actually allocated resources in the roles tree first.
    roleTree.untrackAllocated(slaveId, slave.totalAllocated);

    // Untrack resources in roleTree and sorter.
    foreachpair (
        const FrameworkID& frameworkId,
        const Resources& resources,
        slave.getOfferedOrAllocated()) {
      untrackAllocatedResources(slaveId, frameworkId, resources);
    }

    roleSorter->removeSlave(slaveId);

    foreachvalue (const Owned<Sorter>& sorter, frameworkSorters) {
      sorter->removeSlave(slaveId);
    }

    roleTree.untrackReservations(slave.getTotal().reserved());

    const ResourceQuantities agentScalarQuantities =
        ResourceQuantities::fromScalarResources(slave.getTotal().scalars());

    CHECK_CONTAINS(totalScalarQuantities, agentScalarQuantities);
    totalScalarQuantities -= agentScalarQuantities;
  }

  slaves.erase(slaveId);
  allocationCandidates.erase(slaveId);

  removeFilters(slaveId);

  LOG(INFO) << "Removed agent " << slaveId;
}


void HierarchicalAllocatorProcess::updateSlave(
    const SlaveID& slaveId,
    const SlaveInfo& info,
    const Option<Resources>& total,
    const Option<vector<SlaveInfo::Capability>>& capabilities)
{
  CHECK(initialized);
  CHECK_EQ(slaveId, info.id());

  Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));

  bool updated = false;

  // Remove all offer filters for this slave if it was restarted with changed
  // attributes. We do this because schedulers might have decided that they're
  // not interested in offers from this slave based on the non-presence of some
  // required attributes, and right now they have no other way of learning
  // about this change.
  // TODO(bennoe): Once the agent lifecycle design is implemented, there is a
  // better way to notify frameworks about such changes and let them make this
  // decision. We should think about ways to safely remove this check at that
  // point in time.
  if (!(Attributes(info.attributes()) == Attributes(slave.info.attributes()))) {
    updated = true;
    removeFilters(slaveId);
  }

  if (!(slave.info == info)) {
    updated = true;

    // We unconditionally overwrite the old domain and hostname: Even though
    // the master places some restrictions on this (i.e. agents are not allowed
    // to reregister with a different hostname) inside the allocator it
    // doesn't matter, as the algorithm will work correctly either way.
    slave.info = info;
  }

  // Update agent capabilities.
  if (capabilities.isSome()) {
    protobuf::slave::Capabilities newCapabilities(capabilities.get());
    protobuf::slave::Capabilities oldCapabilities(slave.capabilities);

    slave.capabilities = newCapabilities;

    if (newCapabilities != oldCapabilities) {
      updated = true;

      LOG(INFO) << "Agent " << slaveId << " (" << slave.info.hostname() << ")"
                << " updated with capabilities " << slave.capabilities;
    }
  }

  if (total.isSome()) {
    updated = updateSlaveTotal(slaveId, total.get()) || updated;

    LOG(INFO) << "Agent " << slaveId << " (" << slave.info.hostname() << ")"
              << " updated with total resources " << total.get();
  }

  if (updated) {
    generateOffers(slaveId);
  }
}


void HierarchicalAllocatorProcess::addResourceProvider(
    const SlaveID& slaveId,
    const Resources& total,
    const hashmap<FrameworkID, Resources>& used)
{
  CHECK(initialized);

  Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));
  updateSlaveTotal(slaveId, slave.getTotal() + total);

  foreachpair (const FrameworkID& frameworkId,
               const Resources& allocation,
               used) {
    // There are two cases here:
    //
    //   (1) The framework has already been added to the allocator.
    //       In this case, we track the allocation in the sorters
    //       and the role tree.
    //
    //   (2) The framework has not yet been added to the allocator.
    //       We do not track the resources allocated to this
    //       framework. This leaves a small window where the role
    //       sorting will under-account for the roles belonging
    //       to this framework. This case should never occur since
    //       the master will always add the framework first.
    if (!frameworks.contains(frameworkId)) {
      continue;
    }

    slave.decreaseAvailable(frameworkId, allocation);
    trackAllocatedResources(slaveId, frameworkId, allocation);
  }

  VLOG(1)
    << "Grew agent " << slaveId << " by "
    << total << " (total), "
    << used << " (used)";
}


void HierarchicalAllocatorProcess::removeFilters(const SlaveID& slaveId)
{
  CHECK(initialized);

  foreachvalue (Framework& framework, frameworks) {
    framework.inverseOfferFilters.erase(slaveId);

    // Need a typedef here, otherwise the preprocessor gets confused
    // by the comma in the template argument list.
    typedef hashmap<SlaveID, hashset<shared_ptr<OfferFilter>>> Filters;
    foreachvalue (Filters& filters, framework.offerFilters) {
      filters.erase(slaveId);
    }
  }

  LOG(INFO) << "Removed all filters for agent " << slaveId;
}


void HierarchicalAllocatorProcess::activateSlave(
    const SlaveID& slaveId)
{
  CHECK(initialized);

  Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));
  slave.activated = true;

  LOG(INFO) << "Agent " << slaveId << " reactivated";
}


void HierarchicalAllocatorProcess::deactivateSlave(
    const SlaveID& slaveId)
{
  CHECK(initialized);

  Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));
  slave.activated = false;

  LOG(INFO) << "Agent " << slaveId << " deactivated";
}


void HierarchicalAllocatorProcess::updateWhitelist(
    const Option<hashset<string>>& _whitelist)
{
  CHECK(initialized);

  whitelist = _whitelist;

  if (whitelist.isSome()) {
    LOG(INFO) << "Updated agent whitelist: " << stringify(whitelist.get());

    if (whitelist->empty()) {
      LOG(WARNING) << "Whitelist is empty, no offers will be made!";
    }
  } else {
    LOG(INFO) << "Advertising offers for all agents";
  }
}


void HierarchicalAllocatorProcess::requestResources(
    const FrameworkID& frameworkId,
    const vector<Request>& requests)
{
  CHECK(initialized);

  LOG(INFO) << "Received resource request from framework " << frameworkId;
}


void HierarchicalAllocatorProcess::updateAllocation(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& offeredResources,
    const vector<ResourceConversion>& conversions)
{
  CHECK(initialized);
  CHECK_CONTAINS(frameworks, frameworkId);

  Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));

  // We require that an allocation is tied to a single role.
  //
  // TODO(bmahler): The use of `Resources::allocations()` induces
  // unnecessary copying of `Resources` objects (which is expensive
  // at the time this was written).
  hashmap<string, Resources> allocations = offeredResources.allocations();

  CHECK_EQ(1u, allocations.size());

  string role = allocations.begin()->first;

  Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

  const Resources frameworkAllocation =
    frameworkSorter->allocation(frameworkId.value(), slaveId);

  // We keep a copy of the offered resources here and it is updated
  // by the specified resource conversions.
  //
  // The resources in the resource conversions should have been
  // normalized by the master (contains proper AllocationInfo).
  //
  // TODO(bmahler): Check that the resources in the resource
  // conversions have AllocationInfo set. The master should enforce
  // this. E.g.
  //
  //  foreach (const ResourceConversion& conversion, conversions) {
  //    CHECK_NONE(validateConversionOnAllocatedResources(conversion));
  //  }
  Resources updatedOfferedResources =
    CHECK_NOTERROR(offeredResources.apply(conversions));

  // Update the per-slave allocation.
  slave.increaseAvailable(frameworkId, offeredResources);
  slave.decreaseAvailable(frameworkId, updatedOfferedResources);

  roleTree.untrackOfferedOrAllocated(slaveId, offeredResources);
  roleTree.trackOfferedOrAllocated(slaveId, updatedOfferedResources);

  // Update the allocation in the framework sorter.
  frameworkSorter->update(
      frameworkId.value(),
      slaveId,
      offeredResources,
      updatedOfferedResources);

  // Update the allocation in the role sorter.
  roleSorter->update(
      role,
      slaveId,
      offeredResources,
      updatedOfferedResources);

  // Update the agent total resources so they are consistent with the updated
  // allocation. We do not directly use `updatedOfferedResources` here because
  // the agent's total resources shouldn't contain:
  // 1. The additionally allocated shared resources.
  // 2. `AllocationInfo` as set in `updatedOfferedResources`.
  //
  // We strip `AllocationInfo` from conversions in order to apply them
  // successfully, since agent's total is stored as unallocated resources.
  vector<ResourceConversion> strippedConversions;
  Resources removedResources;
  foreach (const ResourceConversion& conversion, conversions) {
    // TODO(jieyu): Ideally, we should make sure agent's total
    // resources are consistent with agent's allocation in terms of
    // shared resources. In other words, we should increase agent's
    // total resources as well for those additional allocation we did
    // for shared resources. However, that means we need to update the
    // agent's total resources when performing allocation for shared
    // resources (in `__allocate()`). For now, we detect "additional"
    // allocation for shared resources by checking if a conversion has
    // an empty `consumed` field.
    if (conversion.consumed.empty()) {
      continue;
    }

    // NOTE: For now, a resource conversion must either not change the resource
    // quantities, or completely remove the consumed resources. See MESOS-8825.
    if (conversion.converted.empty()) {
      removedResources += conversion.consumed;
    }

    Resources consumed = conversion.consumed;
    Resources converted = conversion.converted;

    consumed.unallocate();
    converted.unallocate();

    strippedConversions.emplace_back(consumed, converted);
  }

  Try<Resources> updatedTotal = slave.getTotal().apply(strippedConversions);
  CHECK_SOME(updatedTotal);

  updateSlaveTotal(slaveId, updatedTotal.get());

  const Resources updatedFrameworkAllocation =
    frameworkSorter->allocation(frameworkId.value(), slaveId);

  // Check that the changed quantities of the framework's allocation is exactly
  // the same as the resources removed by the resource conversions.
  //
  // TODO(chhsiao): Revisit this constraint if we want to support other type of
  // resource conversions. See MESOS-9015.
  const Resources removedAllocationQuantities =
    frameworkAllocation.createStrippedScalarQuantity() -
    updatedFrameworkAllocation.createStrippedScalarQuantity();
  CHECK_EQ(
      removedAllocationQuantities,
      removedResources.createStrippedScalarQuantity());

  LOG(INFO) << "Updated allocation of framework " << frameworkId
            << " on agent " << slaveId
            << " from " << frameworkAllocation
            << " to " << updatedFrameworkAllocation;
}


Future<Nothing> HierarchicalAllocatorProcess::updateAvailable(
    const SlaveID& slaveId,
    const vector<Offer::Operation>& operations)
{
  // Note that the operations may contain allocated resources,
  // however such operations can be applied to unallocated
  // resources unambiguously, so we don't have a strict CHECK
  // for the operations to contain only unallocated resources.

  CHECK(initialized);

  Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));

  // It's possible for this 'apply' to fail here because a call to
  // 'allocate' could have been enqueued by the allocator itself
  // just before master's request to enqueue 'updateAvailable'
  // arrives to the allocator.
  //
  //   Master -------R------------
  //                  \----+
  //                       |
  //   Allocator --A-----A-U---A--
  //                \___/ \___/
  //
  //   where A = allocate, R = reserve, U = updateAvailable
  Try<Resources> updatedAvailable = slave.getAvailable().apply(operations);
  if (updatedAvailable.isError()) {
    VLOG(1) << "Failed to update available resources on agent " << slaveId
            << ": " << updatedAvailable.error();
    return Failure(updatedAvailable.error());
  }

  // Update the total resources.
  Try<Resources> updatedTotal = slave.getTotal().apply(operations);
  CHECK_SOME(updatedTotal);

  // Update the total resources in the sorter.
  updateSlaveTotal(slaveId, updatedTotal.get());

  return Nothing();
}


void HierarchicalAllocatorProcess::updateUnavailability(
    const SlaveID& slaveId,
    const Option<Unavailability>& unavailability)
{
  CHECK(initialized);

  Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));

  // NOTE: We currently implement maintenance in the allocator to be able to
  // leverage state and features such as the FrameworkSorter and OfferFilter.

  // We explicitly remove all filters for the inverse offers of this slave. We
  // do this because we want to force frameworks to reassess the calculations
  // they have made to respond to the inverse offer. Unavailability of a slave
  // can have a large effect on failure domain calculations and inter-leaved
  // unavailability schedules.
  foreachvalue (Framework& framework, frameworks) {
    framework.inverseOfferFilters.erase(slaveId);
  }

  // Remove any old unavailability.
  slave.maintenance = None();

  // If we have a new unavailability.
  if (unavailability.isSome()) {
    slave.maintenance = Slave::Maintenance(unavailability.get());
  }

  generateOffers(slaveId);
}


void HierarchicalAllocatorProcess::updateInverseOffer(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const Option<UnavailableResources>& unavailableResources,
    const Option<InverseOfferStatus>& status,
    const Option<Filters>& filters)
{
  CHECK(initialized);

  Framework& framework = *CHECK_NOTNONE(getFramework(frameworkId));
  Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));

  CHECK(slave.maintenance.isSome())
    << "Agent " << slaveId
    << " (" << slave.info.hostname() << ") should have maintenance scheduled";

  // NOTE: We currently implement maintenance in the allocator to be able to
  // leverage state and features such as the FrameworkSorter and OfferFilter.

  // We use a reference by alias because we intend to modify the
  // `maintenance` and to improve readability.
  Slave::Maintenance& maintenance = slave.maintenance.get();

  // Only handle inverse offers that we currently have outstanding. If it is not
  // currently outstanding this means it is old and can be safely ignored.
  if (maintenance.offersOutstanding.contains(frameworkId)) {
    // We always remove the outstanding offer so that we will send a new offer
    // out the next time we schedule inverse offers.
    maintenance.offersOutstanding.erase(frameworkId);

    // If the response is `Some`, this means the framework responded. Otherwise
    // if it is `None` the inverse offer timed out or was rescinded.
    if (status.isSome()) {
      // For now we don't allow frameworks to respond with `UNKNOWN`. The caller
      // should guard against this. This goes against the pattern of not
      // checking external invariants; however, the allocator and master are
      // currently so tightly coupled that this check is valuable.
      CHECK_NE(status->status(), InverseOfferStatus::UNKNOWN);

      // If the framework responded, we update our state to match.
      maintenance.statuses[frameworkId].CopyFrom(status.get());
    }
  }

  // No need to install filters if `filters` is none.
  if (filters.isNone()) {
    return;
  }

  // Create a refused inverse offer filter.
  Try<Duration> timeout = Duration::create(Filters().refuse_seconds());

  if (filters->refuse_seconds() > Days(365).secs()) {
    LOG(WARNING) << "Using 365 days to create the refused inverse offer"
                 << " filter because the input value is too big";

    timeout = Days(365);
  } else if (filters->refuse_seconds() < 0) {
    LOG(WARNING) << "Using the default value of 'refuse_seconds' to create"
                 << " the refused inverse offer filter because the input"
                 << " value is negative";

    timeout = Duration::create(Filters().refuse_seconds());
  } else {
    timeout = Duration::create(filters->refuse_seconds());

    if (timeout.isError()) {
      LOG(WARNING) << "Using the default value of 'refuse_seconds' to create"
                   << " the refused inverse offer filter because the input"
                   << " value is invalid: " + timeout.error();

      timeout = Duration::create(Filters().refuse_seconds());
    }
  }

  CHECK_SOME(timeout);

  if (timeout.get() != Duration::zero()) {
    VLOG(1) << "Framework " << frameworkId
            << " filtered inverse offers from agent " << slaveId
            << " for " << timeout.get();

    // Create a new inverse offer filter and delay its expiration.
    shared_ptr<RefusedInverseOfferFilter> inverseOfferFilter =
      make_shared<RefusedInverseOfferFilter>(*timeout);

    framework.inverseOfferFilters[slaveId].insert(inverseOfferFilter);

    weak_ptr<InverseOfferFilter> weakPtr = inverseOfferFilter;

    inverseOfferFilter->expired()
      .onReady(defer(self(), [=](Nothing) {
        expire(frameworkId, slaveId, weakPtr);
      }));
  }
}


Future<hashmap<SlaveID, hashmap<FrameworkID, InverseOfferStatus>>>
HierarchicalAllocatorProcess::getInverseOfferStatuses()
{
  CHECK(initialized);

  hashmap<SlaveID, hashmap<FrameworkID, InverseOfferStatus>> result;

  // Make a copy of the most recent statuses.
  foreachpair (const SlaveID& id, const Slave& slave, slaves) {
    if (slave.maintenance.isSome()) {
      result[id] = slave.maintenance->statuses;
    }
  }

  return result;
}


void HierarchicalAllocatorProcess::transitionOfferedToAllocated(
    const SlaveID& slaveId,
    const Resources& resources)
{
  CHECK_NOTNONE(getSlave(slaveId))->totalAllocated += resources;
  roleTree.trackAllocated(slaveId, resources);
}


void HierarchicalAllocatorProcess::recoverResources(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources,
    const Option<Filters>& filters,
    bool isAllocated)
{
  CHECK(initialized);

  if (resources.empty()) {
    return;
  }

  Option<Slave*> slave = getSlave(slaveId);

  if (isAllocated && slave.isSome()) {
    CHECK_CONTAINS((*slave)->totalAllocated, resources);
    (*slave)->totalAllocated -= resources;
    roleTree.untrackAllocated(slaveId, resources);
  }

  Option<Framework*> framework = getFramework(frameworkId);
  // No work to do if either the framework or the agent no longer exists.
  //
  // The framework may not exist if we dispatched Master::offer before we
  // received MesosAllocatorProcess::removeFramework or
  // MesosAllocatorProcess::deactivateFramework, in which case we will
  // have already recovered all of its resources).
  //
  // The agent may not exist if we dispatched Master::offer before we
  // received `removeSlave`.
  if (framework.isNone() || slave.isNone()) {
    return;
  }

  // For now, we require that resources are recovered within a single
  // allocation role (since filtering in the same manner across roles
  // seems undesirable).
  //
  // TODO(bmahler): The use of `Resources::allocations()` induces
  // unnecessary copying of `Resources` objects (which is expensive
  // at the time this was written).
  hashmap<string, Resources> allocations = resources.allocations();

  CHECK_EQ(1u, allocations.size());

  string role = allocations.begin()->first;

  // Update resources on the agent.

  CHECK((*slave)->getTotalOfferedOrAllocated().contains(resources))
    << "agent " << slaveId << " resources "
    << (*slave)->getTotalOfferedOrAllocated() << " do not contain "
    << resources;

  (*slave)->increaseAvailable(frameworkId, resources);

  VLOG(1) << "Recovered " << resources << " (total: " << (*slave)->getTotal()
          << ", offered or allocated: "
          << (*slave)->getTotalOfferedOrAllocated() << ")"
          << " on agent " << slaveId << " from framework " << frameworkId;

  // Update role tree and sorter.

  Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

  if (frameworkSorter->contains(frameworkId.value())) {
    untrackAllocatedResources(slaveId, frameworkId, resources);
  }

  // No need to install the filter if 'filters' is none.
  if (filters.isNone()) {
    return;
  }

  // Update filters.

  // Create a refused resources filter.
  Try<Duration> timeout = Duration::create(Filters().refuse_seconds());

  if (filters->refuse_seconds() > Days(365).secs()) {
    LOG(WARNING) << "Using 365 days to create the refused resources offer"
                 << " filter because the input value is too big";

    timeout = Days(365);
  } else if (filters->refuse_seconds() < 0) {
    LOG(WARNING) << "Using the default value of 'refuse_seconds' to create"
                 << " the refused resources offer filter because the input"
                 << " value is negative";

    timeout = Duration::create(Filters().refuse_seconds());
  } else {
    timeout = Duration::create(filters->refuse_seconds());

    if (timeout.isError()) {
      LOG(WARNING) << "Using the default value of 'refuse_seconds' to create"
                   << " the refused resources offer filter because the input"
                   << " value is invalid: " + timeout.error();

      timeout = Duration::create(Filters().refuse_seconds());
    }
  }

  CHECK_SOME(timeout);

  if (timeout.get() != Duration::zero()) {
    VLOG(1) << "Framework " << frameworkId
            << " filtered agent " << slaveId
            << " for " << timeout.get();

    // Expire the filter after both an `allocationInterval` and the
    // `timeout` have elapsed. This ensures that the filter does not
    // expire before we perform the next allocation for this agent,
    // see MESOS-4302 for more information.
    //
    // Because the next periodic allocation goes through a dispatch
    // after `allocationInterval`, we do the same for `expire()`
    // (with a helper `_expire()`) to achieve the above.
    //
    // TODO(alexr): If we allocated upon resource recovery
    // (MESOS-3078), we would not need to increase the timeout here.
    timeout = std::max(options.allocationInterval, timeout.get());

    // Create a new filter. Note that we unallocate the resources
    // since filters are applied per-role already.
    Resources unallocated = resources;
    unallocated.unallocate();

    shared_ptr<RefusedOfferFilter> offerFilter =
      make_shared<RefusedOfferFilter>(unallocated, *timeout);

    (*framework)->offerFilters[role][slaveId].insert(offerFilter);

    weak_ptr<OfferFilter> weakPtr = offerFilter;

    offerFilter->expired()
      .onReady(defer(self(), [=](Nothing) {
        expire(frameworkId, role, slaveId, weakPtr);
      }));
  }
}


void HierarchicalAllocatorProcess::suppressRoles(
    Framework& framework, const set<string>& roles)
{
  CHECK(initialized);

  // Deactivating the framework in the sorter is fine as long as
  // SUPPRESS is not parameterized. When parameterization is added,
  // we have to differentiate between the cases here.

  foreach (const string& role, roles) {
    Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

    frameworkSorter->deactivate(framework.frameworkId.value());
    framework.suppressedRoles.insert(role);
    framework.metrics->suppressRole(role);
  }

  // TODO(bmahler): This logs roles that were already suppressed,
  // only log roles that transitioned from unsuppressed -> suppressed.
  LOG(INFO) << "Suppressed offers for roles " << stringify(roles)
            << " of framework " << framework.frameworkId;
}


void HierarchicalAllocatorProcess::suppressOffers(
    const FrameworkID& frameworkId,
    const set<string>& roles_)
{
  Framework& framework = *CHECK_NOTNONE(getFramework(frameworkId));

  const set<string>& roles = roles_.empty() ? framework.roles : roles_;
  suppressRoles(framework, roles);
}


void HierarchicalAllocatorProcess::reviveRoles(
    Framework& framework, const set<string>& roles)
{
  CHECK(initialized);

  framework.inverseOfferFilters.clear();

  foreach (const string& role, roles) {
    framework.offerFilters.erase(role);
  }

  // Activating the framework in the sorter is fine as long as
  // SUPPRESS is not parameterized. When parameterization is added,
  // we may need to differentiate between the cases here.
  foreach (const string& role, roles) {
    Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

    frameworkSorter->activate(framework.frameworkId.value());
    framework.suppressedRoles.erase(role);
    framework.metrics->reviveRole(role);
  }

  // TODO(bmahler): This logs roles that were already unsuppressed,
  // only log roles that transitioned from suppressed -> unsuppressed.
  LOG(INFO) << "Unsuppressed offers and cleared filters for roles "
            << stringify(roles) << " of framework " << framework.frameworkId;
}


void HierarchicalAllocatorProcess::reviveOffers(
    const FrameworkID& frameworkId,
    const set<string>& roles)
{
  CHECK(initialized);

  Framework& framework = *CHECK_NOTNONE(getFramework(frameworkId));

  reviveRoles(framework, roles.empty() ? framework.roles : roles);

  generateOffers();
}


void HierarchicalAllocatorProcess::updateQuota(
    const string& role, const Quota& quota)
{
  CHECK(initialized);

  roleTree.updateQuota(role, quota);
  metrics.updateQuota(role, quota);

  LOG(INFO) << "Updated quota for role '" << role << "', "
            << " guarantees: " << quota.guarantees
            << " limits: " << quota.limits;
}


void HierarchicalAllocatorProcess::updateWeights(
    const vector<WeightInfo>& weightInfos)
{
  CHECK(initialized);

  foreach (const WeightInfo& weightInfo, weightInfos) {
    CHECK(weightInfo.has_role());
    roleTree.updateWeight(weightInfo.role(), weightInfo.weight());
    roleSorter->updateWeight(weightInfo.role(), weightInfo.weight());
  }

  // NOTE: Since weight changes do not result in rebalancing of
  // offered resources, we do not trigger an allocation here; the
  // weight change will be reflected in subsequent allocations.
  //
  // If we add the ability for weight changes to incur a rebalancing
  // of offered resources, then we should trigger that here.
}


void HierarchicalAllocatorProcess::pause()
{
  if (!paused) {
    VLOG(1) << "Allocation paused";

    paused = true;
  }
}


void HierarchicalAllocatorProcess::resume()
{
  if (paused) {
    VLOG(1) << "Allocation resumed";

    paused = false;
  }
}


Future<Nothing> HierarchicalAllocatorProcess::generateOffers()
{
  return generateOffers(slaves.keys());
}


Future<Nothing> HierarchicalAllocatorProcess::generateOffers(
    const SlaveID& slaveId)
{
  hashset<SlaveID> slaves({slaveId});
  return generateOffers(slaves);
}


Future<Nothing> HierarchicalAllocatorProcess::generateOffers(
    const hashset<SlaveID>& slaveIds)
{
  if (paused) {
    VLOG(2) << "Skipped allocation because the allocator is paused";

    return Nothing();
  }

  allocationCandidates |= slaveIds;

  if (offerGeneration.isNone() || !offerGeneration->isPending()) {
    metrics.allocation_run_latency.start();
    offerGeneration = dispatch(self(), &Self::_generateOffers);
  }

  return offerGeneration.get();
}


Nothing HierarchicalAllocatorProcess::_generateOffers()
{
  metrics.allocation_run_latency.stop();

  if (paused) {
    VLOG(2) << "Skipped allocation because the allocator is paused";

    return Nothing();
  }

  ++metrics.allocation_runs;

  Stopwatch stopwatch;
  stopwatch.start();
  metrics.allocation_run.start();

  __generateOffers();

  // NOTE: For now, we implement maintenance inverse offers within the
  // allocator. We leverage the existing timer/cycle of offers to also do any
  // inverse offers generation necessary to satisfy maintenance needs.
  generateInverseOffers();

  metrics.allocation_run.stop();

  VLOG(1) << "Performed allocation for " << allocationCandidates.size()
          << " agents in " << stopwatch.elapsed();

  // Clear the candidates on completion of the allocation run.
  allocationCandidates.clear();

  return Nothing();
}


// TODO(alexr): Consider factoring out the quota allocation logic.
void HierarchicalAllocatorProcess::__generateOffers()
{
  // Compute the offerable resources, per framework:
  //   (1) For reserved resources on the slave, allocate these to a
  //       framework having the corresponding role.
  //   (2) For unreserved resources on the slave, allocate these
  //       to a framework of any role.
  hashmap<FrameworkID, hashmap<string, hashmap<SlaveID, Resources>>> offerable;

  // NOTE: This function can operate on a small subset of
  // `allocationCandidates`, we have to make sure that we don't
  // assume cluster knowledge when summing resources from that set.

  vector<SlaveID> slaveIds;
  slaveIds.reserve(allocationCandidates.size());

  // Filter out non-whitelisted, removed, and deactivated slaves
  // in order not to send offers for them.
  foreach (const SlaveID& slaveId, allocationCandidates) {
    Option<Slave*> slave = getSlave(slaveId);

    if (isWhitelisted(slaveId) && slave.isSome() && (*slave)->activated) {
      slaveIds.push_back(slaveId);
    }
  }

  // Randomize the order in which slaves' resources are allocated.
  //
  // TODO(vinod): Implement a smarter sorting algorithm.
  std::random_shuffle(slaveIds.begin(), slaveIds.end());

  // To enforce quota, we keep track of consumed quota for roles with a
  // non-default quota.
  //
  // NOTE: We build the map here to avoid repetitive aggregation in the
  // allocation loop. But this map will still need to be updated in the
  // allocation loop as we make new allocations.
  //
  // TODO(mzhu): Build and persist this information across allocation cycles in
  // track/untrackAllocatedResources().
  //
  // TODO(mzhu): Ideally, we want the sorter to track consumed quota. It then
  // could use consumed quota instead of allocated resources (the former
  // includes unallocated reservations while the latter does not) to calculate
  // the DRF share. This would help to:
  //
  //   (1) Solve the fairness issue when roles with unallocated
  //       reservations may game the allocator (See MESOS-8299).
  //
  //   (2) Simplify the quota enforcement logic -- the allocator
  //       would no longer need to track reservations separately.
  hashmap<string, ResourceQuantities> rolesConsumedQuota;

  // We only log headroom info if there is any non-default quota set.
  // We set this flag value as we iterate through all roles below.
  //
  // TODO(mzhu): remove this once we can determine if there is any non-default
  // quota set by looking into the allocator memory state in constant time.
  bool logHeadroomInfo = false;

  // We charge a role against its quota by considering its allocation
  // (including all subrole allocations) as well as any unallocated
  // reservations (including all subrole reservations) since reservations
  // are bound to the role. In other words, we always consider reservations
  // as consuming quota, regardless of whether they are allocated.
  // It is calculated as:
  //
  //   Consumed Quota = reservations + unreserved allocation

  // Add reservations and unreserved offeredOrAllocated.
  //
  // Currently, only top level roles can have quota set and thus
  // we only track consumed quota for top level roles.
  foreach (const Role* r, roleTree.root()->children()) {
    // TODO(mzhu): Track all role consumed quota. We may want to expose
    // these as metrics.
    if (r->quota() != DEFAULT_QUOTA) {
      logHeadroomInfo = true;
      rolesConsumedQuota[r->role] += r->quotaOfferedOrConsumed();
    }
  }

  // We need to constantly make sure that we are holding back enough
  // unreserved resources that the remaining quota guarantee can later
  // be satisfied when needed:
  //
  //   Required unreserved headroom =
  //     sum (guarantee - consumed quota) for each role.
  //
  // Given the above, if a role has more reservations (which count towards
  // consumed quota) than quota guarantee, we don't need to hold back any
  // unreserved headroom for it.
  ResourceQuantities requiredHeadroom;
  foreach (const Role* r, roleTree.root()->children()) {
    requiredHeadroom +=
      r->quota().guarantees -
      rolesConsumedQuota.get(r->role).getOrElse(ResourceQuantities());
  }

  // We will allocate resources while ensuring that the required
  // unreserved non-revocable headroom is still available. Otherwise,
  // we will not be able to satisfy the quota guarantee later.
  //
  //   available headroom = unallocated unreserved non-revocable resources
  //
  // We compute this as:
  //
  //   available headroom = total resources -
  //                        allocated resources -
  //                        unallocated reservations -
  //                        unallocated revocable resources
  ResourceQuantities availableHeadroom = totalScalarQuantities;

  // NOTE: The role sorter does not return aggregated allocation
  // information whereas `reservationScalarQuantities` does, so
  // we need to loop over only top level roles for the latter.

  // Subtract allocated resources from the total.
  availableHeadroom -= roleSorter->allocationScalarQuantities();

  // Subtract total unallocated reservations.
  // unallocated reservations = total reservations - allocated reservations
  availableHeadroom -=
    roleTree.root()->reservationScalarQuantities() -
    roleTree.root()->offeredOrAllocatedReservedScalarQuantities();

  // Subtract revocable resources.
  foreachvalue (const Slave& slave, slaves) {
    availableHeadroom -= ResourceQuantities::fromScalarResources(
        slave.getAvailable().revocable().scalars());
  }

  if (logHeadroomInfo) {
    LOG(INFO) << "Before allocation, required quota headroom is "
              << requiredHeadroom
              << " and available quota headroom is " << availableHeadroom;
  }

  // Due to the two stages in the allocation algorithm and the nature of
  // shared resources being re-offerable even if already allocated, the
  // same shared resources can appear in two (and not more due to the
  // `allocatable` check in each stage) distinct offers in one allocation
  // cycle. This is undesirable since the allocator API contract should
  // not depend on its implementation details. For now we make sure a
  // shared resource is only allocated once in one offer cycle. We use
  // `offeredSharedResources` to keep track of shared resources already
  // allocated in the current cycle.
  hashmap<SlaveID, Resources> offeredSharedResources;

  // In the 1st stage, we allocate to roles with non-default quota guarantees.
  //
  // NOTE: Even though we keep track of the available headroom, we still
  // dedicate the first stage for roles with non-default quota guarantees.
  // The reason is that quota guarantees headroom only acts as a quantity
  // guarantee. Frameworks might have filters or capabilities such that the
  // resources set aside for the headroom cannot be used by these frameworks,
  // resulting in unsatisfied guarantees (despite enough headroom set aside).
  // Thus we try to satisfy the quota guarantees in this first stage so that
  // those roles with unsatisfied guarantees can have more choices and higher
  // probability in getting their guarantees satisfied.
  foreach (const SlaveID& slaveId, slaveIds) {
    Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));

    foreach (const string& role, roleSorter->sort()) {
      const Quota& quota = getQuota(role);

      const ResourceQuantities& quotaGuarantees = quota.guarantees;
      const ResourceLimits& quotaLimits = quota.limits;

      // We only allocate to roles with non-default guarantees
      // in the first stage.
      if (quotaGuarantees.empty()) {
        continue;
      }

      // If there are no active frameworks in this role, we do not
      // need to do any allocations for this role.
      bool noFrameworks = [&]() {
        Option<const Role*> r = roleTree.get(role);

        return r.isNone() || (*r)->frameworks().empty();
      }();

      if (noFrameworks) {
        continue;
      }

      // TODO(bmahler): Handle shared volumes, which are always available but
      // should be excluded here based on `offeredSharedResources`.
      if (slave.getAvailable().empty()) {
        break; // Nothing left on this agent.
      }

      ResourceQuantities unsatisfiedQuotaGuarantees =
        quotaGuarantees -
        rolesConsumedQuota.get(role).getOrElse(ResourceQuantities());

      // We only allocate to roles with unsatisfied guarantees
      // in the first stage.
      if (unsatisfiedQuotaGuarantees.empty()) {
        continue;
      }

      // Fetch frameworks in the order provided by the sorter.
      // NOTE: Suppressed frameworks are not included in the sort.
      Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

      foreach (const string& frameworkId_, frameworkSorter->sort()) {
        if (unsatisfiedQuotaGuarantees.empty()) {
          break;
        }
        FrameworkID frameworkId;
        frameworkId.set_value(frameworkId_);

        const Framework& framework = *CHECK_NOTNONE(getFramework(frameworkId));
        CHECK(framework.active) << frameworkId;

        if (framework.offerConstraintsFilter.isAgentExcluded(
                role, slave.info)) {
          // Framework filters the agent regardless of remaining resources.
          continue;
        }

        // Offer a shared resource only if it has not been offered in this
        // offer cycle to a framework.
        Resources available =
          slave.getAvailable().allocatableTo(role) -
          offeredSharedResources.get(slaveId).getOrElse(Resources());

        if (available.empty()) {
          break; // Nothing left for the role.
        }


        // An early `continue` optimization.
        if (!allocatable(available, role, framework)) {
          continue;
        }

        if (!isCapableOfReceivingAgent(framework.capabilities, slave)) {
          continue;
        }

        available = stripIncapableResources(available, framework.capabilities);

        // In this first stage, we allocate the role's reservations as well as
        // any unreserved resources while enforcing the role's quota limits and
        // the global headroom. We'll "chop" the unreserved resources if needed.
        //
        // E.g. A role has no allocations or reservations yet and a 10 cpu
        //      quota limits. We'll chop a 15 cpu agent down to only
        //      allocate 10 cpus to the role to keep it within its limits.
        //
        // Note on bursting above guarantees up to the limits in the 1st stage:
        //
        // In this 1st stage, for resources that the role has non-default
        // guarantees, we allow the role to burst above this guarantee up to
        // its limit (while maintaining the global headroom). In addition,
        // if the role is allocated any resources that help it to make
        // progress towards its quota guarantees, or the role is being
        // allocated some reservation(s), we will also allocate all of the
        // resources (subject to its limits and global headroom) for which it
        // does not have any guarantees for.
        //
        // E.g. The agent has 1 cpu, 1024 mem, 1024 disk, 1 gpu, 5 ports
        //      and the role has guarantees for 1 cpu, 512 mem and no limits.
        //      We'll include all the disk, gpu, and ports in the allocation,
        //      despite the role not having any quota guarantee for them. In
        //      addition, we will also allocate all the 1024 mem to the role.
        //
        // Rationale of allocating all non-guaranteed resources on the agent
        // (subject to role limits and global headroom requirements):
        //
        // Currently, it is not possible to set quota on non-scalar resources,
        // like ports. A user may also only choose to set guarantees on some
        // scalar resources (e.g. on cpu but not on memory). If we do not
        // allocate these resources together with the guarantees, frameworks
        // will get non-usable offers (e.g. with no ports or memory).
        // However, the downside of this approach is that, after one allocation,
        // the agent will be deprived of some resources (e.g. no ports),
        // rendering any subsequent offers non-usable. Users are advised to
        // leverage the `min_allocatbale_resources` to help prevent such offers
        // and reduce resource fragmentation.
        //
        // Rationale of allowing roles to burst scalar resource allocations up
        // to its limits (subject to headroom requirements) in this first stage:
        //
        // Allowing roles to burst in this first stage would help to reduce
        // fragmentation--guaranteed resources and non-guarantee bursting
        // resources are combined into one offer from one agent. However,
        // the downside is that, such premature bursting will may prevent
        // subsequent roles from getting guarantees, especially if their
        // frameworks are picky. This is true despite the enforced headroom
        // which only enforces quantity. Nevertheless, We choose to allow
        // such bursting for less resource fragmentation.

        // Resources that can be used to to increase a role's quota consumption.
        //
        // This is hot path, we use explicit filter calls to avoid
        // multiple traversal.
        Resources quotaResources =
          available.filter([&](const Resource& resource) {
            return resource.type() == Value::SCALAR &&
                   Resources::isUnreserved(resource) &&
                   !Resources::isRevocable(resource);
          });

        Resources guaranteesOffering =
          shrinkResources(quotaResources, unsatisfiedQuotaGuarantees);

        // We allocate this agent only if the role can make progress towards
        // its quota guarantees i.e. it is getting some unreserved resources
        // for its guarantees . Otherwise, this role is not going to get any
        // allocation. We can safely continue here.
        //
        // NOTE: For roles with unallocated reservations on this agent, if
        // its guarantees are already satisfied or this agent has no resources
        // that can contribute to its guarantees (except the reservation), we
        // will also skip it here. Its reservations will be allocated in the
        // second stage.
        //
        // NOTE: Since we currently only support top-level roles to
        // have quota, there are no ancestor reservations involved here.
        if (guaranteesOffering.empty()) {
          continue;
        }

        // This role's reservations, non-scalar resources and revocable
        // resources, as well as guarantees are always allocated.
        //
        // We need to allocate guarantees unconditionally here so that
        // even the cluster is overcommitted by guarantees (thus deficit in
        // headroom), this role's guarantees can still be allocated.
        Resources toOffer = guaranteesOffering +
                               available.filter([&](const Resource& resource) {
                                 return Resources::isReserved(resource, role) ||
                                        resource.type() != Value::SCALAR ||
                                        Resources::isRevocable(resource);
                               });

        Resources additionalScalarOffering =
          quotaResources - guaranteesOffering;

        // Then, non-guaranteed quota resources are subject to quota limits
        // and global headroom enforcements.

        // Limits enforcement.
        if (!quotaLimits.empty()) {
          additionalScalarOffering = shrinkResources(
              additionalScalarOffering,
              quotaLimits - CHECK_NOTNONE(rolesConsumedQuota.get(role)) -
                ResourceQuantities::fromScalarResources(guaranteesOffering));
        }

        // Headroom enforcement.
        //
        // This check is only for performance optimization.
        if (!requiredHeadroom.empty() && !additionalScalarOffering.empty()) {
          // Shrink down to surplus headroom.
          //
          // Surplus headroom = (availableHeadroom - guaranteesOffering) -
          //                      (requiredHeadroom - guaranteesOffering)
          //                  = availableHeadroom - requiredHeadroom
          additionalScalarOffering = shrinkResources(
              additionalScalarOffering, availableHeadroom - requiredHeadroom);
        }

        toOffer += additionalScalarOffering;

        // If the framework filters these resources, ignore.
        if (!allocatable(toOffer, role, framework) ||
            isFiltered(framework, role, slave, toOffer)) {
          continue;
        }

        VLOG(2) << "Offering " << toOffer << " on agent " << slaveId
                << " to role " << role << " of framework " << frameworkId
                << " as part of its role quota";

        toOffer.allocate(role);

        offerable[frameworkId][role][slaveId] += toOffer;
        offeredSharedResources[slaveId] += toOffer.shared();

        // Update role consumed quota and quota headroom.

        ResourceQuantities increasedQuotaConsumption =
          ResourceQuantities::fromScalarResources(
              guaranteesOffering + additionalScalarOffering);

        unsatisfiedQuotaGuarantees -= increasedQuotaConsumption;
        rolesConsumedQuota[role] += increasedQuotaConsumption;
        for (const string& ancestor : roles::ancestors(role)) {
          rolesConsumedQuota[ancestor] += increasedQuotaConsumption;
        }

        requiredHeadroom -=
          ResourceQuantities::fromScalarResources(guaranteesOffering);
        availableHeadroom -= increasedQuotaConsumption;

        slave.decreaseAvailable(frameworkId, toOffer);

        trackAllocatedResources(slaveId, frameworkId, toOffer);
      }
    }
  }

  // Similar to the first stage, we will allocate resources while ensuring
  // that the required unreserved non-revocable headroom is still available
  // for unsatisfied quota guarantees. Otherwise, we will not be able to
  // satisfy quota guarantees later. Reservations and revocable resources
  // will always be included in the offers since allocating these does not
  // make progress towards satisifying quota guarantees.
  //
  // For logging purposes, we track the number of agents that had resources
  // held back for quota headroom, as well as how many resources in total
  // were held back.
  //
  // While we also held resources back for quota headroom in the first stage,
  // we do not track it there. This is because in the second stage, we try to
  // allocate all resources (including the ones held back in the first stage).
  // Thus only resources held back in the second stage are truly held back for
  // the whole allocation cycle.
  ResourceQuantities heldBackForHeadroom;
  size_t heldBackAgentCount = 0;

  // We randomize the agents here to "spread out" the effect of the first
  // stage, which tends to allocate from the front of the agent list more
  // so than the back.
  std::random_shuffle(slaveIds.begin(), slaveIds.end());

  foreach (const SlaveID& slaveId, slaveIds) {
    Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));

    foreach (const string& role, roleSorter->sort()) {
      // TODO(bmahler): Handle shared volumes, which are always available but
      // should be excluded here based on `offeredSharedResources`.
      if (slave.getAvailable().empty()) {
        break; // Nothing left on this agent.
      }

      const ResourceLimits& quotaLimits = getQuota(role).limits;

      // NOTE: Suppressed frameworks are not included in the sort.
      Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

      foreach (const string& frameworkId_, frameworkSorter->sort()) {
        FrameworkID frameworkId;
        frameworkId.set_value(frameworkId_);

        const Framework& framework = *CHECK_NOTNONE(getFramework(frameworkId));

        if (framework.offerConstraintsFilter.isAgentExcluded(
                role, slave.info)) {
          // Framework filters the agent regardless of remaining resources.
          continue;
        }

        // Offer a shared resource only if it has not been offered in this
        // offer cycle to a framework.
        Resources available =
          slave.getAvailable().allocatableTo(role) -
          offeredSharedResources.get(slaveId).getOrElse(Resources());

        if (available.empty()) {
          break; // Nothing left for the role.
        }


        // An early `continue` optimization.
        if (!allocatable(available, role, framework)) {
          continue;
        }

        if (!isCapableOfReceivingAgent(framework.capabilities, slave)) {
          continue;
        }

        available = stripIncapableResources(available, framework.capabilities);

        // Reservations (including the roles ancestors' reservations),
        // non-scalar resources and revocable resources are always allocated.
        Resources toOffer = available.filter([&](const Resource& resource) {
          return Resources::isReserved(resource) ||
                 resource.type() != Value::SCALAR ||
                 Resources::isRevocable(resource);
        });

        // Then, unreserved scalar resources are subject to quota limits
        // and global headroom enforcement.
        //
        // This is hot path, we use explicit filter calls to avoid
        // multiple traversal.
        Resources additionalScalarOffering =
          available.filter([&](const Resource& resource) {
            return resource.type() == Value::SCALAR &&
                   Resources::isUnreserved(resource) &&
                   !Resources::isRevocable(resource);
          });

        // Limits enforcement.
        if (!quotaLimits.empty()) {
          additionalScalarOffering = shrinkResources(
              additionalScalarOffering,
              quotaLimits - CHECK_NOTNONE(rolesConsumedQuota.get(role)));
        }

        // Headroom enforcement.
        //
        // This check is only for performance optimization.
        if (!requiredHeadroom.empty() && !additionalScalarOffering.empty()) {
          Resources shrunk = shrinkResources(
              additionalScalarOffering, availableHeadroom - requiredHeadroom);

          // If resources are held back.
          if (shrunk != additionalScalarOffering) {
            heldBackForHeadroom += ResourceQuantities::fromScalarResources(
                additionalScalarOffering - shrunk);
            ++heldBackAgentCount;

            additionalScalarOffering = std::move(shrunk);
          }
        }

        toOffer += additionalScalarOffering;

        // If the framework filters these resources, ignore.
        if (!allocatable(toOffer, role, framework) ||
            isFiltered(framework, role, slave, toOffer)) {
          continue;
        }

        VLOG(2) << "Offering " << toOffer << " on agent " << slaveId
                << " to role " << role << " of framework " << frameworkId;

        toOffer.allocate(role);

        offerable[frameworkId][role][slaveId] += toOffer;
        offeredSharedResources[slaveId] += toOffer.shared();

        // Update role consumed quota and quota headroom

        ResourceQuantities increasedQuotaConsumption =
          ResourceQuantities::fromScalarResources(additionalScalarOffering);

        if (getQuota(role) != DEFAULT_QUOTA) {
          rolesConsumedQuota[role] += increasedQuotaConsumption;
          for (const string& ancestor : roles::ancestors(role)) {
            rolesConsumedQuota[ancestor] += increasedQuotaConsumption;
          }
        }

        availableHeadroom -= increasedQuotaConsumption;

        slave.decreaseAvailable(frameworkId, toOffer);

        trackAllocatedResources(slaveId, frameworkId, toOffer);
      }
    }
  }

  if (logHeadroomInfo) {
    LOG(INFO) << "After allocation, " << requiredHeadroom
              << " are required for quota headroom, "
              << heldBackForHeadroom << " were held back from "
              << heldBackAgentCount
              << " agents to ensure sufficient quota headroom";
  }

  if (offerable.empty()) {
    VLOG(2) << "No allocations performed";
  } else {
    // Now offer the resources to each framework.
    foreachkey (const FrameworkID& frameworkId, offerable) {
      offerCallback(frameworkId, offerable.at(frameworkId));
    }
  }
}


void HierarchicalAllocatorProcess::generateInverseOffers()
{
  // In this case, `offerable` is actually the slaves and/or resources that we
  // want the master to create `InverseOffer`s from.
  hashmap<FrameworkID, hashmap<SlaveID, UnavailableResources>> offerable;

  // For maintenance, we only send inverse offers to frameworks that have the
  // potential to lose something (i.e. it has resources offered or allocated on
  // a given agent). We keep track of which frameworks already have an
  // outstanding inverse offer for the given slave in the UnavailabilityStatus
  // of the specific slave using the `offerOutstanding` flag. This is equivalent
  // to the accounting we do for resources when we send regular offers. If we
  // didn't keep track of outstanding offers then we would keep generating new
  // inverse offers even though the framework had not responded yet.
  //
  // TODO(mzhu): Need to consider reservations as well.
  foreach (const SlaveID& slaveId, allocationCandidates) {
    Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));

    if (slave.maintenance.isSome()) {
      // We use a reference by alias because we intend to modify the
      // `maintenance` and to improve readability.
      Slave::Maintenance& maintenance = slave.maintenance.get();

      foreachkey (
          const FrameworkID& frameworkId, slave.getOfferedOrAllocated()) {
        const Option<Framework*> framework_ = getFramework(frameworkId);
        // NOTE: This method might be called in-between adding per-framework
        // used resources on an agent via `addSlave()` and adding a framework
        // via `addFramework()`.
        if (framework_.isNone()) {
          // Framework is using resources on an agent but has not yet been
          // re-added to allocator via addFramework().
          continue;
        }

        const Framework& framework = **framework_;

        // No need to deallocate for an inactive framework as the master
        // will not send it inverse offers.
        if (!framework.active) {
          continue;
        }

        // If this framework doesn't already have inverse offers for the
        // specified slave.
        if (!offerable[frameworkId].contains(slaveId)) {
          // If there isn't already an outstanding inverse offer to this
          // framework for the specified slave.
          if (!maintenance.offersOutstanding.contains(frameworkId)) {
            // Ignore in case the framework filters inverse offers for this
            // slave.
            //
            // NOTE: Since this specific allocator implementation only sends
            // inverse offers for maintenance primitives, and those are at the
            // whole slave level, we only need to filter based on the
            // time-out.
            if (isFiltered(framework, slave)) {
              continue;
            }

            const UnavailableResources unavailableResources =
              UnavailableResources{Resources(), maintenance.unavailability};

            // For now we send inverse offers with empty resources when the
            // inverse offer represents maintenance on the machine. In the
            // future we could be more specific about the resources on the
            // host, as we have the information available.
            offerable[frameworkId][slaveId] = unavailableResources;

            // Mark this framework as having an offer outstanding for the
            // specified slave.
            maintenance.offersOutstanding.insert(frameworkId);
          }
        }
      }
    }
  }

  if (offerable.empty()) {
    VLOG(2) << "No inverse offers to send out!";
  } else {
    // Now send inverse offers to each framework.
    foreachkey (const FrameworkID& frameworkId, offerable) {
      inverseOfferCallback(frameworkId, offerable[frameworkId]);
    }
  }
}


void HierarchicalAllocatorProcess::_expire(
    const FrameworkID& frameworkId,
    const string& role,
    const SlaveID& slaveId,
    const weak_ptr<OfferFilter>& offerFilter)
{
  // The filter might have already been removed (e.g., if the
  // framework no longer exists or in `reviveOffers()`) but
  // we may land here if the cancelation of the expiry timeout
  // did not succeed (due to the dispatch already being in the
  // queue).
  shared_ptr<OfferFilter> filter = offerFilter.lock();

  if (filter.get() == nullptr) {
    return;
  }

  // Since this is a performance-sensitive piece of code,
  // we use find to avoid the doing any redundant lookups.
  auto frameworkIterator = frameworks.find(frameworkId);
  CHECK(frameworkIterator != frameworks.end());

  Framework& framework = frameworkIterator->second;

  auto roleFilters = framework.offerFilters.find(role);
  CHECK(roleFilters != framework.offerFilters.end());

  auto agentFilters = roleFilters->second.find(slaveId);
  CHECK(agentFilters != roleFilters->second.end());

  // Erase the filter (may be a no-op per the comment above).
  agentFilters->second.erase(filter);
  if (agentFilters->second.empty()) {
    roleFilters->second.erase(slaveId);
  }
  if (roleFilters->second.empty()) {
    framework.offerFilters.erase(role);
  }
}


void HierarchicalAllocatorProcess::expire(
    const FrameworkID& frameworkId,
    const string& role,
    const SlaveID& slaveId,
    const weak_ptr<OfferFilter>& offerFilter)
{
  dispatch(
      self(),
      &Self::_expire,
      frameworkId,
      role,
      slaveId,
      offerFilter);
}


void HierarchicalAllocatorProcess::expire(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const weak_ptr<InverseOfferFilter>& inverseOfferFilter)
{
  // The filter might have already been removed (e.g., if the
  // framework no longer exists or in
  // HierarchicalAllocatorProcess::reviveOffers) but
  // we may land here if the cancelation of the expiry timeout
  // did not succeed (due to the dispatch already being in the
  // queue).
  shared_ptr<InverseOfferFilter> filter = inverseOfferFilter.lock();

  if (filter.get() == nullptr) {
    return;
  }

  // Since this is a performance-sensitive piece of code,
  // we use find to avoid the doing any redundant lookups.
  auto frameworkIterator = frameworks.find(frameworkId);
  CHECK(frameworkIterator != frameworks.end());

  Framework& framework = frameworkIterator->second;

  auto filters = framework.inverseOfferFilters.find(slaveId);
  CHECK(filters != framework.inverseOfferFilters.end());

  filters->second.erase(filter);
  if (filters->second.empty()) {
    framework.inverseOfferFilters.erase(slaveId);
  }
}


bool HierarchicalAllocatorProcess::isWhitelisted(
    const SlaveID& slaveId) const
{
  return whitelist.isNone() ||
    whitelist->contains(CHECK_NOTNONE(getSlave(slaveId))->info.hostname());
}


bool HierarchicalAllocatorProcess::isFiltered(
    const Framework& framework,
    const string& role,
    const Slave& slave,
    const Resources& resources) const
{
  // TODO(mpark): Consider moving these filter logic out and into the master,
  // since they are not specific to the hierarchical allocator but rather are
  // global allocation constraints.

  // Prevent offers from non-MULTI_ROLE agents to be allocated
  // to MULTI_ROLE frameworks.
  if (framework.capabilities.multiRole &&
      !slave.capabilities.multiRole) {
    LOG(WARNING) << "Implicitly filtering agent " << slave.info.id()
                 << " from framework " << framework.frameworkId
                 << " because the framework is MULTI_ROLE capable"
                 << " but the agent is not";

    return true;
  }

  // Prevent offers from non-HIERARCHICAL_ROLE agents to be allocated
  // to hierarchical roles.
  if (!slave.capabilities.hierarchicalRole && strings::contains(role, "/")) {
    LOG(WARNING) << "Implicitly filtering agent " << slave.info.id()
                 << " from role " << role
                 << " because the role is hierarchical but the agent is not"
                 << " HIERARCHICAL_ROLE capable";

    return true;
  }

  // Since this is a performance-sensitive piece of code,
  // we use find to avoid the doing any redundant lookups.
  auto roleFilters = framework.offerFilters.find(role);
  if (roleFilters == framework.offerFilters.end()) {
    return false;
  }

  auto agentFilters = roleFilters->second.find(slave.info.id());
  if (agentFilters == roleFilters->second.end()) {
    return false;
  }

  foreach (const shared_ptr<OfferFilter>& offerFilter, agentFilters->second) {
    if (offerFilter->filter(resources)) {
      VLOG(1) << "Filtered offer with " << resources
              << " on agent " << slave.info.id()
              << " for role " << role
              << " of framework " << framework.frameworkId;

      return true;
    }
  }

  return false;
}


bool HierarchicalAllocatorProcess::isFiltered(
    const Framework& framework, const Slave& slave) const
{
  if (framework.inverseOfferFilters.contains(slave.info.id())) {
    foreach (const shared_ptr<InverseOfferFilter>& inverseOfferFilter,
             framework.inverseOfferFilters.at(slave.info.id())) {
      if (inverseOfferFilter->filter()) {
        VLOG(1) << "Filtered unavailability on agent " << slave.info.id()
                << " for framework " << framework.frameworkId;

        return true;
      }
    }
  }

  return false;
}


bool HierarchicalAllocatorProcess::allocatable(
    const Resources& resources,
    const string& role,
    const Framework& framework) const
{
  if (resources.empty()) {
    return false;
  }

  // By default we check against the globally configured minimal
  // allocatable resources.
  //
  // NOTE: We use a pointer instead of `Option` semantics here to
  // avoid copying vectors in code in the hot path of the allocator.
  const vector<ResourceQuantities>* _minAllocatableResources =
    options.minAllocatableResources.isSome()
      ? &options.minAllocatableResources.get()
      : nullptr;

  if (framework.minAllocatableResources.contains(role)) {
    _minAllocatableResources = &framework.minAllocatableResources.at(role);
  }

  // If no minimal requirements or an empty set of requirments are
  // configured any resource is allocatable.
  if (_minAllocatableResources == nullptr ||
      _minAllocatableResources->empty()) {
    return true;
  }

  return std::any_of(
      _minAllocatableResources->begin(),
      _minAllocatableResources->end(),
      [&](const ResourceQuantities& qs) { return resources.contains(qs); });
}


double HierarchicalAllocatorProcess::_resources_offered_or_allocated(
    const string& resource)
{
  double offered_or_allocated = 0;

  foreachvalue (const Slave& slave, slaves) {
    Option<Value::Scalar> value =
      slave.getTotalOfferedOrAllocated().get<Value::Scalar>(resource);

    if (value.isSome()) {
      offered_or_allocated += value->value();
    }
  }

  return offered_or_allocated;
}


double HierarchicalAllocatorProcess::_resources_total(
    const string& resource)
{
  return totalScalarQuantities.get(resource).value();
}


double HierarchicalAllocatorProcess::_quota_offered_or_allocated(
    const string& role,
    const string& resource)
{
  if (!roleSorter->contains(role)) {
    // This can occur when execution of this callback races with removal of the
    // metric for a role which does not have any associated frameworks.
    return 0.;
  }

  return roleSorter->allocationScalarQuantities(role).get(resource).value();
}


double HierarchicalAllocatorProcess::_offer_filters_active(
    const string& role)
{
  double result = 0;

  foreachvalue (const Framework& framework, frameworks) {
    if (!framework.offerFilters.contains(role)) {
      continue;
    }

    foreachkey (const SlaveID& slaveId, framework.offerFilters.at(role)) {
      result += framework.offerFilters.at(role).at(slaveId).size();
    }
  }

  return result;
}


bool HierarchicalAllocatorProcess::isFrameworkTrackedUnderRole(
    const FrameworkID& frameworkId, const string& role) const
{
  Option<const Role*> r = roleTree.get(role);
  return r.isSome() && (*r)->frameworks().contains(frameworkId);
}


Option<Slave*> HierarchicalAllocatorProcess::getSlave(
    const SlaveID& slaveId) const
{
  auto it = slaves.find(slaveId);

  if (it == slaves.end()) return None();

  return const_cast<Slave*>(&it->second);
}


Option<Framework*> HierarchicalAllocatorProcess::getFramework(
    const FrameworkID& frameworkId) const
{
  auto it = frameworks.find(frameworkId);

  if (it == frameworks.end()) return None();

  return const_cast<Framework*>(&it->second);
}


Option<Sorter*> HierarchicalAllocatorProcess::getFrameworkSorter(
    const string& role) const
{
  auto it = frameworkSorters.find(role);

  if (it == frameworkSorters.end()) return None();

  return const_cast<Sorter*>(it->second.get());
}


const Quota& HierarchicalAllocatorProcess::getQuota(const string& role) const
{
  Option<const Role*> r = roleTree.get(role);

  return r.isSome() ? (*r)->quota() : DEFAULT_QUOTA;
}


void HierarchicalAllocatorProcess::trackFrameworkUnderRole(
    const Framework& framework, const string& role)
{
  CHECK(initialized);

  // If this is the first framework to subscribe to this role,
  // initialize state as necessary.
  if (roleTree.get(role).isNone() ||
      (*roleTree.get(role))->frameworks().empty()) {
    CHECK_NOT_CONTAINS(*roleSorter, role);
    roleSorter->add(role);
    roleSorter->activate(role);

    CHECK_NOT_CONTAINS(frameworkSorters, role);
    frameworkSorters.insert({role, Owned<Sorter>(frameworkSorterFactory())});

    Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

    frameworkSorter->initialize(options.fairnessExcludeResourceNames);

    foreachvalue (const Slave& slave, slaves) {
      frameworkSorter->addSlave(
          slave.info.id(),
          ResourceQuantities::fromScalarResources(slave.getTotal().scalars()));
    }
  }

  roleTree.trackFramework(framework.frameworkId, role);

  Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

  CHECK_NOT_CONTAINS(*frameworkSorter, framework.frameworkId.value())
    << " for role " << role;
  frameworkSorter->add(framework.frameworkId.value());
}


bool HierarchicalAllocatorProcess::tryUntrackFrameworkUnderRole(
    const Framework& framework, const string& role)
{
  CHECK(initialized);

  Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));
  CHECK_CONTAINS(*frameworkSorter, framework.frameworkId.value())
    << " for role " << role;

  if (!frameworkSorter->allocation(framework.frameworkId.value()).empty()) {
    return false;
  }

  roleTree.untrackFramework(framework.frameworkId, role);

  frameworkSorter->remove(framework.frameworkId.value());

  if (roleTree.get(role).isNone() ||
      (*roleTree.get(role))->frameworks().empty()) {
    CHECK_EQ(frameworkSorter->count(), 0u);
    roleSorter->remove(role);
    frameworkSorters.erase(role);
  }

  return true;
}


bool HierarchicalAllocatorProcess::updateSlaveTotal(
    const SlaveID& slaveId,
    const Resources& total)
{
  Slave& slave = *CHECK_NOTNONE(getSlave(slaveId));

  const Resources oldTotal = slave.getTotal();

  if (oldTotal == total) {
    return false;
  }

  slave.updateTotal(total);

  roleTree.untrackReservations(oldTotal.reserved());
  roleTree.trackReservations(total.reserved());

  // Update the total in the allocator and totals in the sorters.
  const ResourceQuantities oldAgentScalarQuantities =
    ResourceQuantities::fromScalarResources(oldTotal.scalars());

  const ResourceQuantities agentScalarQuantities =
    ResourceQuantities::fromScalarResources(total.scalars());

  CHECK_CONTAINS(totalScalarQuantities, oldAgentScalarQuantities);
  totalScalarQuantities -= oldAgentScalarQuantities;
  totalScalarQuantities += agentScalarQuantities;

  roleSorter->removeSlave(slaveId);
  roleSorter->addSlave(slaveId, agentScalarQuantities);

  foreachvalue (const Owned<Sorter>& sorter, frameworkSorters) {
    sorter->removeSlave(slaveId);
    sorter->addSlave(slaveId, agentScalarQuantities);
  }

  return true;
}


bool HierarchicalAllocatorProcess::isRemoteSlave(const Slave& slave) const
{
  // If the slave does not have a configured domain, assume it is not remote.
  if (!slave.info.has_domain()) {
    return false;
  }

  // The current version of the Mesos agent refuses to startup if a
  // domain is specified without also including a fault domain. That
  // might change in the future, if more types of domains are added.
  // For forward compatibility, we treat agents with a configured
  // domain but no fault domain as having no configured domain.
  if (!slave.info.domain().has_fault_domain()) {
    return false;
  }

  // If the slave has a configured domain (and it has been allowed to
  // register with the master), the master must also have a configured
  // domain.
  CHECK(options.domain.isSome());

  // The master will not startup if configured with a domain but no
  // fault domain.
  CHECK(options.domain->has_fault_domain());

  const DomainInfo::FaultDomain::RegionInfo& masterRegion =
    options.domain->fault_domain().region();
  const DomainInfo::FaultDomain::RegionInfo& slaveRegion =
    slave.info.domain().fault_domain().region();

  return masterRegion != slaveRegion;
}


bool HierarchicalAllocatorProcess::isCapableOfReceivingAgent(
    const protobuf::framework::Capabilities& frameworkCapabilities,
    const Slave& slave) const
{
  // Only offer resources from slaves that have GPUs to
  // frameworks that are capable of receiving GPUs.
  // See MESOS-5634.
  if (options.filterGpuResources && !frameworkCapabilities.gpuResources &&
      slave.hasGpu()) {
    return false;
  }

  // If this framework is not region-aware, don't offer it
  // resources on agents in remote regions.
  if (!frameworkCapabilities.regionAware && isRemoteSlave(slave)) {
    return false;
  }

  return true;
}


Resources HierarchicalAllocatorProcess::stripIncapableResources(
    const Resources& resources,
    const protobuf::framework::Capabilities& frameworkCapabilities) const
{
  return resources.filter([&](const Resource& resource) {
    if (!frameworkCapabilities.sharedResources &&
        Resources::isShared(resource)) {
      return false;
    }

    if (!frameworkCapabilities.revocableResources &&
        Resources::isRevocable(resource)) {
      return false;
    }

    // When reservation refinements are present, old frameworks without the
    // RESERVATION_REFINEMENT capability won't be able to understand the
    // new format. While it's possible to translate the refined reservations
    // into the old format by "hiding" the intermediate reservations in the
    // "stack", this leads to ambiguity when processing RESERVE / UNRESERVE
    // operations. This is due to the loss of information when we drop the
    // intermediate reservations. Therefore, for now we simply filter out
    // resources with refined reservations if the framework does not have
    // the capability.
    if (!frameworkCapabilities.reservationRefinement &&
        Resources::hasRefinedReservations(resource)) {
      return false;
    }

    return true;
  });
}


void HierarchicalAllocatorProcess::trackAllocatedResources(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const Resources& allocated)
{
  CHECK_CONTAINS(slaves, slaveId);
  CHECK_CONTAINS(frameworks, frameworkId);

  // TODO(bmahler): Calling allocations() is expensive since it has
  // to construct a map. Avoid this.
  foreachpair (const string& role,
               const Resources& allocation,
               allocated.allocations()) {
    // The framework has resources allocated to this role but it may
    // or may not be subscribed to the role. Either way, we need to
    // track the framework under the role.
    if (!isFrameworkTrackedUnderRole(frameworkId, role)) {
      trackFrameworkUnderRole(*CHECK_NOTNONE(getFramework(frameworkId)), role);
    }

    CHECK_CONTAINS(*roleSorter, role);

    Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

    CHECK_CONTAINS(*frameworkSorter, frameworkId.value())
      << " for role " << role;

    roleTree.trackOfferedOrAllocated(slaveId, allocation);

    roleSorter->allocated(role, slaveId, allocation);
    frameworkSorter->allocated(
        frameworkId.value(), slaveId, allocation);
  }
}


void HierarchicalAllocatorProcess::untrackAllocatedResources(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const Resources& allocated)
{
  // TODO(mzhu): Add a `CHECK(slaves.contains(slaveId));`
  // here once MESOS-621 is resolved. Ideally, `removeSlave()`
  // should unallocate resources in the framework sorters.
  // But currently, a slave is removed first via `removeSlave()`
  // and later a call to `recoverResources()` occurs to recover
  // the framework's resources.
  Framework* framework = CHECK_NOTNONE(getFramework(frameworkId));

  // TODO(bmahler): Calling allocations() is expensive since it has
  // to construct a map. Avoid this.
  foreachpair (const string& role,
               const Resources& allocation,
               allocated.allocations()) {
    CHECK_CONTAINS(*roleSorter, role);

    Sorter* frameworkSorter = CHECK_NOTNONE(getFrameworkSorter(role));

    CHECK_CONTAINS(*frameworkSorter, frameworkId.value())
      << "for role " << role;

    roleTree.untrackOfferedOrAllocated(slaveId, allocation);

    frameworkSorter->unallocated(frameworkId.value(), slaveId, allocation);

    roleSorter->unallocated(role, slaveId, allocation);

    // If the framework is no longer subscribed to the role, we can try to
    // untrack the framework under the role.
    if (framework->roles.count(role) == 0) {
      tryUntrackFrameworkUnderRole(*framework, role);
    }
  }
}


static string OFFER_CONSTRAINTS_DEBUG_HELP()
{
  return process::HELP(
    process::TLDR(
        "Evaluates current framework offer constraints and returns results."),
    process::DESCRIPTION(
        "This endpoint evaluates for each role of each framework a list",
        "of agents excluded from allocation by offer constraints,",
        "and also reports the current set of framework's suppressed roles",
        "",
        "Example:",
        "```",
        "{\"frameworks\": {",
        "   \"0f4c63a9-be1e-4a90-9e11-d7bf0aa6c8ad-0017\": {",
        "     \"suppressed_roles\": [\"role2\"]",
        "     \"excluded_by_attribute_constraints\": {",
        "       \"role1\": [",
        "         \"0b1e7d60-dfbc-44c9-8222-48b57eca8637-S123\",",
        "         \"654af69c-80f7-45ad-bcb3-c7c917f1811b-S045\"],",
        "       \"role2\": [] }},",
        "   \"b0377da6-090d-4338-9e2e-bf6cf0f309b7-0011\": {",
        "     \"suppressed_roles\": []",
        "     \"excluded_by_attribute_constraints\": { \"role1\": [] }}",
        "}}",
        "```",
        "In this example, two agents are excluded from allocation",
        "to the first framework (-0017) under the role \"role1\", no agents",
        "are excluded from allocation to this framework under \"role2\".",
        "In addition, this framework has \"role2\" suppressed.",
        "The second framework (-0011) is also subscribed to \"role1\",",
        "but has no agents excluded from allocation to \"role1\";",
        "this framework has no roles currently suppressed."
        ),
    process::AUTHENTICATION(true),
    process::AUTHORIZATION(
        "This endpoint skips frameworks for which the user is not authorized"
        "to perform a VIEW_FRAMEWORK action."));
}


Future<process::http::Response>
HierarchicalAllocatorProcess::offerConstraintsDebug(
    const process::http::Request&,
    const Option<process::http::authentication::Principal>& principal)
{
  if (options.authorizer.isNone()) {
    return offerConstraintsDebug_(nullptr);
  }

  return (*options.authorizer)
    ->getApprover(
        authorization::createSubject(principal),
        authorization::Action::VIEW_FRAMEWORK)
    .then(defer(self(), &Self::offerConstraintsDebug_, lambda::_1));
}


process::http::Response HierarchicalAllocatorProcess::offerConstraintsDebug_(
    shared_ptr<const ObjectApprover> frameworksApprover)
{
  vector<const Framework*> approvedFrameworks;
  foreachvalue (const Framework& framework, frameworks) {
    Try<bool> approved = frameworksApprover
                           ? frameworksApprover->approved(framework.info)
                           : true;

    if (approved.isError()) {
      LOG(WARNING) << "Error authorizing VIEW_FRAMEWORK for framework "
                   << framework.info.id() << ": " << approved.error();

      return process::http::InternalServerError(
          "Failed to authorize VIEW_FRAMEWORK: " + approved.error());
    }

    if (*approved) {
      approvedFrameworks.push_back(&framework);
    }
  }

  auto writeFrameworks = [&](JSON::ObjectWriter* writer) {
    for (const Framework* framework : approvedFrameworks) {
      auto writeFramework = [&](JSON::ObjectWriter* writer) {
        writer->field("suppressed_roles", [&](JSON::ArrayWriter* writer) {
          for (const string& role : framework->suppressedRoles) {
            writer->element(role);
          }
        });

        writer->field(
            "excluded_by_attribute_constraints",
            [&](JSON::ObjectWriter* writer) {
              for (const string& role : framework->roles) {
                writer->field(role, [&](JSON::ArrayWriter* writer) {
                  foreachvalue (const Slave& slave, slaves) {
                    if (framework->offerConstraintsFilter.isAgentExcluded(
                            role, slave.info)) {
                      writer->element(stringify(slave.id));
                    }
                  }
                });
              }
            });
      };

      writer->field(stringify(framework->info.id()), writeFramework);
    }
  };

  return process::http::OK(jsonify([&](JSON::ObjectWriter* writer) {
    writer->field("frameworks", writeFrameworks);
  }));
}


} // namespace internal {
} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {
