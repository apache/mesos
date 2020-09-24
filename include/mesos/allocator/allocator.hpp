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

#ifndef __MESOS_ALLOCATOR_ALLOCATOR_HPP__
#define __MESOS_ALLOCATOR_ALLOCATOR_HPP__

#include <string>
#include <vector>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include <mesos/allocator/allocator.pb.h>
#include <mesos/scheduler/scheduler.pb.h>

#include <mesos/authorizer/authorizer.hpp>
#include <mesos/maintenance/maintenance.hpp>
#include <mesos/quota/quota.hpp>
#include <mesos/resource_quantities.hpp>
#include <mesos/resources.hpp>

#include <process/future.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>


namespace mesos {
namespace allocator {

constexpr Duration DEFAULT_ALLOCATOR_RECOVERY_TIMEOUT = Minutes(10);
constexpr double DEFAULT_ALLOCATOR_AGENT_RECOVERY_FACTOR = 0.80;


/**
 *  Pass in configuration to the allocator.
 */
struct Options
{
  Duration allocationInterval = Seconds(1);

  // Resources (by name) that will be excluded from a role's fair share.
  Option<std::set<std::string>> fairnessExcludeResourceNames = None();

  // Filter GPU resources based on the `GPU_RESOURCES` framework capability.
  bool filterGpuResources = true;

  // The master's domain, if any.
  Option<DomainInfo> domain = None();

  // The minimum allocatable resource quantities, if any.
  Option<std::vector<ResourceQuantities>> minAllocatableResources = None();

  size_t maxCompletedFrameworks = 0;

  bool publishPerFrameworkMetrics = true;

  // Authentication realm for HTTP debug endpoints exposed by the allocator.
  Option<std::string> readonlyHttpAuthenticationRealm;

  // Mesos master's authorizer.
  Option<::mesos::Authorizer*> authorizer;

  // Recovery options
  Duration recoveryTimeout = DEFAULT_ALLOCATOR_RECOVERY_TIMEOUT;
  double agentRecoveryFactor = DEFAULT_ALLOCATOR_AGENT_RECOVERY_FACTOR;
};


namespace internal {
class OfferConstraintsFilterImpl;

} // namespace internal {


class OfferConstraintsFilter
{
public:
  // TODO(asekretenko): Given that most dependants of the public allocator
  // interface do not care about filter creation, we should consider decoupling
  // the filter construction interface (which at this point consists of the
  // `struct Options` and the `create()` method) from the allocator interface.
  struct Options
  {
    struct RE2Limits
    {
      Bytes maxMem;
      int maxProgramSize;
    };

    RE2Limits re2Limits;
  };

  static Try<OfferConstraintsFilter> create(
      const Options& options,
      scheduler::OfferConstraints&& constraints);

  /*
   * Constructs a no-op filter that does not exclude any agents/resources from
   * being offered. This is equivalent to passing default-constructed
   * `OfferConstraints` to the factory method `create()`.
   */
  OfferConstraintsFilter();

  // Definitions of these need `OfferConstraintsFilterImpl` to be a complete
  // type.
  OfferConstraintsFilter(OfferConstraintsFilter&&);
  OfferConstraintsFilter& operator=(OfferConstraintsFilter&&);
  ~OfferConstraintsFilter();

  /**
   * Returns `true` if the allocator is allowed to offer resoureces
   * on the agent to the framework's role, and `false` otherwise.
   */
  bool isAgentExcluded(
      const std::string& role,
      const SlaveInfo& agentInfo) const;

  // TODO(asekretenko): Add a method for filtering `Resources` on an agent.

private:
  std::unique_ptr<internal::OfferConstraintsFilterImpl> impl;

  OfferConstraintsFilter(internal::OfferConstraintsFilterImpl&& impl_);
};


/**
 * Per-framework allocator-specific options that are not part of
 * `FrameworkInfo`.
 */
struct FrameworkOptions
{
  /**
   * The set of roles for which the allocator should not generate offers.
   */
  std::set<std::string> suppressedRoles;

  /**
   * The internal representation of framework's offer constraints.
   */
  OfferConstraintsFilter offerConstraintsFilter;
};


/**
 * Basic model of an allocator: resources are allocated to a framework
 * in the form of offers. A framework can refuse some resources in
 * offers and run tasks in others. Allocated resources can have offer
 * operations applied to them in order for frameworks to alter the
 * resource metadata (e.g. creating persistent volumes). Resources can
 * be recovered from a framework when tasks finish/fail (or are lost
 * due to an agent failure) or when an offer is rescinded.
 *
 * This is the public API for resource allocators.
 */
class Allocator
{
public:
  /**
   * Attempts either to create a built-in DRF allocator or to load an
   * allocator instance from a module using the given name. If `Try`
   * does not report an error, the wrapped `Allocator*` is not null.
   *
   * TODO(bmahler): Figure out how to pass parameters without
   * burning in the built-in module arguments.
   *
   * @param name Name of the allocator.
   */
  static Try<Allocator*> create(
      const std::string& name,
      const std::string& roleSorter,
      const std::string& frameworkSorter);

  Allocator() {}

  virtual ~Allocator() {}

  /**
   * Initializes the allocator when the master starts up. Any errors in
   * initialization should fail fast and result in an ABORT. The master expects
   * the allocator to be successfully initialized if this call returns.
   *
   * @param allocationInterval The allocate interval for the allocator, it
   *     determines how often the allocator should perform the batch
   *     allocation. An allocator may also perform allocation based on events
   *     (a framework is added and so on), this depends on the implementation.
   * @param offerCallback A callback the allocator uses to send allocations
   *     to the frameworks.
   * @param inverseOfferCallback A callback the allocator uses to send reclaim
   *     allocations from the frameworks.
   */
  virtual void initialize(
      const Options& options,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<std::string, hashmap<SlaveID, Resources>>&)>&
                   offerCallback,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, UnavailableResources>&)>&
        inverseOfferCallback) = 0;

  /**
   * Informs the allocator of the recovered state from the master.
   *
   * Because it is hard to define recovery for a running allocator, this
   * method should be called after `initialize()`, but before actual
   * allocation starts (i.e. `addSlave()` is called).
   *
   * TODO(alexr): Consider extending the signature with expected
   * frameworks count once it is available upon the master failover.
   *
   * @param quotas A (possibly empty) collection of quotas, keyed by
   *     their role, known to the master.
   */
  virtual void recover(
      const int expectedAgentCount,
      const hashmap<std::string, Quota>& quotas) = 0;

  /**
   * Adds a framework to the Mesos cluster. The allocator is invoked when
   * a new framework joins the Mesos cluster and is entitled to participate
   * in resource sharing.
   *
   * @param used Resources used by this framework. The allocator should
   *     account for these resources when updating the allocation of this
   *     framework. The allocator should avoid double accounting when yet
   *     unknown agents are added later in `addSlave()`.
   *
   * @param active Whether the framework is initially activated.
   *
   * @param options Initial FrameworkOptions of this framework.
   */
  virtual void addFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const hashmap<SlaveID, Resources>& used,
      bool active,
      FrameworkOptions&& options) = 0;

  /**
   * Removes a framework from the Mesos cluster. It is up to an allocator to
   * decide what to do with framework's resources. For example, they may be
   * released and added back to the shared pool of resources.
   */
  virtual void removeFramework(
      const FrameworkID& frameworkId) = 0;

  /**
   * Activates a framework in the Mesos cluster.
   * Offers are only sent to active frameworks.
   */
  virtual void activateFramework(
      const FrameworkID& frameworkId) = 0;

   /**
   * Deactivates a framework in the Mesos cluster.
   * Resource offers are not sent to deactivated frameworks.
   */
  virtual void deactivateFramework(
      const FrameworkID& frameworkId) = 0;

  /**
   * Updates FrameworkInfo and FrameworkOptions.
   *
   * NOTE: Effective framework options can also be changed via other methods.
   * For example, `suppress()` and `revive()` change the suppression state
   * of framework's roles.
   */
  virtual void updateFramework(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      FrameworkOptions&& options) = 0;

  /**
   * Adds or re-adds an agent to the Mesos cluster. It is invoked when a
   * new agent joins the cluster or in case of agent recovery.
   *
   * @param slaveId ID of the agent to be added or re-added.
   * @param slaveInfo Detailed info of the agent. The slaveInfo resources
   *     correspond directly to the static --resources flag value on the agent.
   * @param capabilities Capabilities of the agent.
   * @param total The `total` resources are passed explicitly because it
   *     includes resources that are dynamically "checkpointed" on the agent
   *     (e.g. persistent volumes, dynamic reservations, etc).
   * @param used Resources that are allocated on the current agent. The
   *     allocator should avoid double accounting when yet unknown frameworks
   *     are added later in `addFramework()`.
   *
   * TODO(asekretenko): Ideally, to get rig of an intermediate allocator state
   * when some resources are used by nonexistent frameworks, we should change
   * the interface so that per-agent per-framework used resources and the
   * not yet known frameworks that are using them are added atomically.
   */
  virtual void addSlave(
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const std::vector<SlaveInfo::Capability>& capabilities,
      const Option<Unavailability>& unavailability,
      const Resources& total,
      const hashmap<FrameworkID, Resources>& used) = 0;

  /**
   * Removes an agent from the Mesos cluster. All resources belonging to this
   * agent should be released by the allocator.
   */
  virtual void removeSlave(
      const SlaveID& slaveId) = 0;

  /**
   * Updates an agent.
   *
   * TODO(bevers): Make `total` and `capabilities` non-optional.
   *
   * @param slaveInfo The current slave info of the agent.
   * @param total The new total resources on the agent.
   * @param capabilities The new capabilities of the agent.
   */
  virtual void updateSlave(
      const SlaveID& slave,
      const SlaveInfo& slaveInfo,
      const Option<Resources>& total = None(),
      const Option<std::vector<SlaveInfo::Capability>>&
          capabilities = None()) = 0;

  /**
   * Add resources from a local resource provider to an agent.
   *
   * @param slave Id of the agent to modify.
   * @param total The resources to add to the agent's total resources.
   * @param used The resources to add to the resources tracked as used
   *     for this agent.
   */
  virtual void addResourceProvider(
      const SlaveID& slave,
      const Resources& total,
      const hashmap<FrameworkID, Resources>& used) = 0;

  /**
   * Activates an agent. This is invoked when an agent reregisters. Offers
   * are only sent for activated agents.
   */
  virtual void activateSlave(
      const SlaveID& slaveId) = 0;

  /**
   * Deactivates an agent.
   *
   * This is triggered if an agent disconnects from the master. The allocator
   * should treat all offers from the deactivated agent as rescinded. (There
   * is no separate call to the allocator to handle this). Resources aren't
   * "recovered" when an agent deactivates because the resources are lost.
   */
  virtual void deactivateSlave(
      const SlaveID& slaveId) = 0;

  /**
   * Updates the list of trusted agents.
   *
   * This is invoked when the master starts up with the --whitelist flag.
   *
   * @param whitelist A set of agents that are allowed to contribute
   *     their resources to the resource pool.
   */
  virtual void updateWhitelist(
      const Option<hashset<std::string>>& whitelist) = 0;

  /**
   * Requests resources for a framework.
   *
   * A framework may request resources via this call. It is up to the allocator
   * how to react to this request. For example, a request may be ignored, or
   * may influence internal priorities the allocator may keep for frameworks.
   */
  virtual void requestResources(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests) = 0;

  /**
   * Updates allocation by applying offer operations.
   *
   * This call is mainly intended to support persistence-related features
   * (dynamic reservation and persistent volumes). The allocator may react
   * differently for certain offer operations. The allocator should use this
   * call to update bookkeeping information related to the framework. The
   * `offeredResources` are the resources that the operations are applied to
   * and must be allocated to a single role.
   */
  virtual void updateAllocation(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& offeredResources,
      const std::vector<ResourceConversion>& conversions) = 0;

  /**
   * Updates available resources on an agent based on a sequence of offer
   * operations. Operations may include reserve, unreserve, create or destroy.
   *
   * @param slaveId ID of the agent.
   * @param operations The offer operations to apply to this agent's resources.
   */
  virtual process::Future<Nothing> updateAvailable(
      const SlaveID& slaveId,
      const std::vector<Offer::Operation>& operations) = 0;

  /**
   * Updates unavailability for an agent.
   *
   * We currently support storing the next unavailability, if there is one,
   * per agent. If `unavailability` is not set then there is no known upcoming
   * unavailability. This might require the implementation of the function to
   * remove any inverse offers that are outstanding.
   */
  virtual void updateUnavailability(
      const SlaveID& slaveId,
      const Option<Unavailability>& unavailability) = 0;

  /**
   * Updates inverse offer.
   *
   * Informs the allocator that the inverse offer has been responded to or
   * revoked.
   *
   * @param unavailableResources The `unavailableResources` can be used by the
   *     allocator to distinguish between different inverse offers sent to the
   *     same framework for the same slave.
   * @param status If `status` is not set then the inverse offer was not
   *     responded to, possibly because the offer timed out or was rescinded.
   *     This might require the implementation of the function to remove any
   *     inverse offers that are outstanding.
   * @param filters A filter attached to the inverse offer can be used by the
   *     framework to control when it wants to be contacted again with the
   *     inverse offer. The "filters" for InverseOffers are identical to the
   *     existing mechanism for re-offering Offers to frameworks.
   */
  virtual void updateInverseOffer(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const Option<UnavailableResources>& unavailableResources,
      const Option<InverseOfferStatus>& status,
      const Option<Filters>& filters = None()) = 0;

  /**
   * Retrieves the status of all inverse offers maintained by the allocator.
   */
  virtual process::Future<
      hashmap<SlaveID,
              hashmap<FrameworkID, mesos::allocator::InverseOfferStatus>>>
    getInverseOfferStatuses() = 0;

  /**
   * This method should be invoked when the offered resources has become
   * actually allocated.
   */
  virtual void transitionOfferedToAllocated(
      const SlaveID& slaveId, const Resources& resources) = 0;

  /**
   * Recovers resources.
   *
   * Used to update the set of available resources for a specific agent. This
   * method is invoked to inform the allocator about offered resources that
   * have been refused or allocated (i.e. used for launching tasks) resources
   * that are no longer in use. The resources will have an
   * `allocation_info.role` assigned and callers are expected to only call this
   * with resources allocated to a single role.
   *
   *
   * TODO(bmahler): We could allow resources allocated to multiple roles
   * within a single call here, but filtering them in the same way does
   * not seem desirable.
   */
  virtual void recoverResources(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources,
      const Option<Filters>& filters,
      bool isAllocated) = 0;

  /**
   * Suppresses offers.
   *
   * Informs the allocator to stop sending offers to this framework for the
   * specified roles. If `roles` is an empty set, we will stop sending offers
   * to this framework for all of the framework's subscribed roles.
   */
  virtual void suppressOffers(
      const FrameworkID& frameworkId,
      const std::set<std::string>& roles) = 0;

  /**
   * Revives offers to this framework for the specified roles. This is
   * invoked by a framework when it wishes to receive filtered resources
   * immediately or get itself out of a suppressed state. If `roles` is
   * an empty set, it is treated as being set to all of the framework's
   * subscribed roles.
   */
  virtual void reviveOffers(
      const FrameworkID& frameworkId,
      const std::set<std::string>& roles) = 0;

  /**
   * Informs the allocator to update quota for the given role.
   *
   * It is up to the allocator implementation how to satisfy quota. An
   * implementation may employ different strategies for roles with or
   * without quota. All roles have a default quota defined as `DEFAULT_QUOTA`.
   * Currently, it is no guarantees and no limits. Thus to "remove" a quota,
   * one should simply update the quota to be `DEFAULT_QUOTA`.
   */
  virtual void updateQuota(
      const std::string& role,
      const Quota& quota) = 0;

  /**
   * Updates the weight associated with one or more roles. If a role
   * was previously configured to have a weight and that role is
   * omitted from this list, it keeps its old weight.
   */
  virtual void updateWeights(
      const std::vector<WeightInfo>& weightInfos) = 0;

  /**
   * Idempotent helper to pause allocations.
   */
  virtual void pause() = 0;

  /**
   * Idempotent helper to resume allocations.
   */
  virtual void resume() = 0;
};

} // namespace allocator {
} // namespace mesos {

#endif // __MESOS_MASTER_ALLOCATOR_HPP__
