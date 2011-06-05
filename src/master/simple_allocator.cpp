#include <algorithm>

#include <glog/logging.h>

#include "simple_allocator.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;

using std::max;
using std::sort;


void SimpleAllocator::frameworkAdded(Framework* framework)
{
  LOG(INFO) << "Added " << framework;
  makeNewOffers();
}


void SimpleAllocator::frameworkRemoved(Framework* framework)
{
  LOG(INFO) << "Removed " << framework;
  foreachpair (Slave* s, unordered_set<Framework*>& refs, refusers)
    refs.erase(framework);
  // TODO: Re-offer just the slaves that the framework had tasks on?
  //       Alternatively, comment this out and wait for a timer tick
  makeNewOffers();
}


void SimpleAllocator::slaveAdded(Slave* slave)
{
  LOG(INFO) << "Added " << slave << " with \n"
            << Resources(slave->info.resources());
  refusers[slave] = unordered_set<Framework*>();
  totalResources += slave->info.resources();
  makeNewOffers(slave);
}


void SimpleAllocator::slaveRemoved(Slave* slave)
{
  LOG(INFO) << "Removed " << slave;
  totalResources -= slave->info.resources();
  refusers.erase(slave);
}


void SimpleAllocator::taskRemoved(Task* task, TaskRemovalReason reason)
{
  LOG(INFO) << "Removed " << task;
  // Remove all refusers from this slave since it has more resources free
  Slave* slave = master->lookupSlave(task->slave_id());
  CHECK(slave != 0);
  refusers[slave].clear();
  // Re-offer the resources, unless this task was removed due to a lost
  // slave or a lost framework (in which case we'll get another callback)
  if (reason == TRR_TASK_ENDED || reason == TRR_EXECUTOR_LOST)
    makeNewOffers(slave);
}


void SimpleAllocator::offerReturned(SlotOffer* offer,
                                    OfferReturnReason reason,
                                    const vector<SlaveResources>& resLeft)
{
  LOG(INFO) << "Offer returned: " << offer << ", reason = " << reason;

  // If this offer returned due to the framework replying, add it to refusers.
  if (reason == ORR_FRAMEWORK_REPLIED) {
    Framework* framework = master->lookupFramework(offer->frameworkId);
    CHECK(framework != 0);
    foreach (const SlaveResources& r, resLeft) {
      VLOG(1) << "Framework reply leaves " << r.resources.allocatable()
              << " free on " << r.slave;
      if (r.resources.allocatable().size() > 0) {
        VLOG(1) << "Inserting " << framework << " as refuser for " << r.slave;
        refusers[r.slave].insert(framework);
      }
    }
  }

  // Make new offers unless the offer returned due to a lost framework or slave
  // (in those cases, frameworkRemoved and slaveRemoved will be called later),
  // or returned due to a framework failover (in which case the framework's
  // new PID won't be set yet so we just wait for the next timer tick).
  if (reason != ORR_SLAVE_LOST && reason != ORR_FRAMEWORK_LOST &&
      reason != ORR_FRAMEWORK_FAILOVER) {
    vector<Slave*> slaves;
    foreach (const SlaveResources& r, resLeft)
      slaves.push_back(r.slave);
    makeNewOffers(slaves);
  }
}


void SimpleAllocator::offersRevived(Framework* framework)
{
  LOG(INFO) << "Filters removed for " << framework;
  makeNewOffers();
}


void SimpleAllocator::timerTick()
{
  // TODO: Is this necessary?
  makeNewOffers();
}


namespace {
  
struct DominantShareComparator
{
  DominantShareComparator(const Resources& _resources)
    : resources(_resources) {}
  
  bool operator () (Framework* f1, Framework* f2)
  {
    double share1 = 0;
    double share2 = 0;

    // TODO(benh): This implementaion of "dominant resource fairness"
    // currently does not take into account resources that are not
    // scalars.

    foreach (const Resource& resource, resources) {
      if (resource.type() == Resource::SCALAR) {
        double total = resource.scalar().value();

        if (total > 0) {
          const Resource::Scalar& scalar1 =
            f1->resources.getScalar(resource.name(), Resource::Scalar());
          share1 = max(share1, scalar1.value() / total);

          const Resource::Scalar& scalar2 =
            f2->resources.getScalar(resource.name(), Resource::Scalar());
          share2 = max(share2, scalar2.value() / total);
        }
      }
    }

    if (share1 == share2)
      // Make the sort deterministic for unit testing.
      return f1->frameworkId.value() < f2->frameworkId.value();
    else
      return share1 < share2;
  }

  Resources resources;
};

}


vector<Framework*> SimpleAllocator::getAllocationOrdering()
{
  vector<Framework*> frameworks = master->getActiveFrameworks();
  DominantShareComparator comp(totalResources);
  sort(frameworks.begin(), frameworks.end(), comp);
  return frameworks;
}


void SimpleAllocator::makeNewOffers()
{
  // TODO: Create a method in master so that we don't return the whole list of slaves
  vector<Slave*> slaves = master->getActiveSlaves();
  makeNewOffers(slaves);
}


void SimpleAllocator::makeNewOffers(Slave* slave)
{
  vector<Slave*> slaves;
  slaves.push_back(slave);
  makeNewOffers(slaves);
}


void SimpleAllocator::makeNewOffers(const vector<Slave*>& slaves)
{
  // Get an ordering of frameworks to send offers to
  vector<Framework*> ordering = getAllocationOrdering();
  if (ordering.size() == 0) {
    VLOG(1) << "makeNewOffers returning because no frameworks are connected";
    return;
  }
  
  // Find all the free resources that can be allocated.
  unordered_map<Slave* , Resources> freeResources;
  foreach (Slave* slave, slaves) {
    if (slave->active) {
      Resources resources = slave->resourcesFree();
      Resources allocatable = resources.allocatable();

      // TODO(benh): For now, only make offers when there is some cpu
      // and memory left. This is an artifact of the original code
      // that only offered when there was at least 1 cpu "unit"
      // available, and without doing this a framework might get
      // offered resources with only memory available (which it
      // obviously won't take) and then get added as a refuser for
      // that slave and therefore have to wait upwards of
      // DEFAULT_REFUSAL_TIMEOUT until resources come from that slave
      // again. In the long run, frameworks will poll the master for
      // resources, rather than the master pushing resources out to
      // frameworks.

      Resource::Scalar cpus;
      cpus.set_value(0);

      cpus = allocatable.getScalar("cpus", cpus);

      Resource::Scalar mem;
      mem.set_value(0);

      mem = allocatable.getScalar("mem", mem);

      if (cpus.value() >= MIN_CPUS && mem.value() > MIN_MEM) {
        VLOG(1) << "Found free resources: " << allocatable << " on " << slave;
        freeResources[slave] = allocatable;
      }
    }
  }

  if (freeResources.size() == 0) {
    VLOG(1) << "makeNewOffers returning because there are no free resources";
    return;
  }
  
  // Clear refusers on any slave that has been refused by everyone
  foreachpair (Slave* slave, _, freeResources) {
    unordered_set<Framework*>& refs = refusers[slave];
    if (refs.size() == ordering.size()) {
      VLOG(1) << "Clearing refusers for " << slave
              << " because everyone refused it";
      refs.clear();
    }
  }

  foreach (Framework* framework, ordering) {
    // See which resources this framework can take (given filters & refusals)
    vector<SlaveResources> offerable;
    foreachpair (Slave* slave, const Resources& resources, freeResources) {
      if (refusers[slave].find(framework) == refusers[slave].end() &&
          !framework->filters(slave, resources)) {
        VLOG(1) << "Offering " << resources << " on " << slave
                << " to framework " << framework->frameworkId;
        offerable.push_back(SlaveResources(slave, resources));
      }
    }

    if (offerable.size() > 0) {
      foreach (SlaveResources& r, offerable) {
        freeResources.erase(r.slave);
      }

      master->makeOffer(framework, offerable);
    }
  }
}
