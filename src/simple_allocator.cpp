#include <algorithm>

#include <glog/logging.h>

#include "simple_allocator.hpp"


using std::max;
using std::sort;

using namespace nexus;
using namespace nexus::internal;
using namespace nexus::internal::master;


void SimpleAllocator::frameworkAdded(Framework *framework)
{
  LOG(INFO) << "Added " << framework;
  makeNewOffers();
}


void SimpleAllocator::frameworkRemoved(Framework *framework)
{
  LOG(INFO) << "Removed " << framework;
  foreachpair (Slave *s, unordered_set<Framework *>& refs, refusers)
    refs.erase(framework);
  makeNewOffers();
}


void SimpleAllocator::slaveAdded(Slave *slave)
{
  LOG(INFO) << "Added " << slave;
  refusers[slave] = unordered_set<Framework *>();
  makeNewOffers();
}


void SimpleAllocator::slaveRemoved(Slave *slave)
{
  LOG(INFO) << "Removed " << slave;
  refusers.erase(slave);
}


void SimpleAllocator::taskRemoved(Task *task, TaskRemovalReason reason)
{
  LOG(INFO) << "Removed " << task;
  // Remove all refusers from this slave since it has more resources free
  Slave *slave = master->lookupSlave(task->slaveId);
  CHECK(slave != 0);
  refusers[slave].clear();
  // Re-offer the resources, unless this task was removed due to a lost
  // slave or a lost framework (in which case we'll get another callback)
  if (reason == TRR_TASK_ENDED) {
    // TODO: Use a more efficient makeOffers() that re-offers just one slave?
    makeNewOffers();
  }
}


void SimpleAllocator::offerReturned(SlotOffer* offer,
                                    OfferReturnReason reason,
                                    const vector<SlaveResources>& resLeft)
{
  LOG(INFO) << "Offer returned: " << offer << ", reason = " << reason;
  // If this offer returned due to the framework replying, add it to refusers
  if (reason == ORR_FRAMEWORK_REPLIED) {
    Framework *framework = master->lookupFramework(offer->frameworkId);
    CHECK(framework != 0);
    foreach (const SlaveResources& r, resLeft) {
      LOG(INFO) << "Framework reply leaves " << r.resources 
                << " free on " << r.slave;
      if (r.resources.cpus > 0 || r.resources.mem > 0) {
        LOG(INFO) << "Inserting " << framework << " as refuser for " << r.slave;
        refusers[r.slave].insert(framework);
      }
    }
  }
  // Make new offers, unless the offer returned due to a lost framework or slave
  // (in those cases, frameworkRemoved and slaveRemoved will be called later)
  // TODO: Use a more efficient makeOffers() that looks only at resLeft?
  if (reason != ORR_SLAVE_LOST && reason != ORR_FRAMEWORK_LOST)
    makeNewOffers();
}


void SimpleAllocator::offersRevived(Framework *framework)
{
  LOG(INFO) << "Filters removed for " << framework;
  makeNewOffers();
}


void SimpleAllocator::timerTick()
{
  makeNewOffers();
}


namespace {
  
struct DominantShareComparator
{
  Resources total;
  
  DominantShareComparator(Resources _total) : total(_total)
  {
    if (total.cpus == 0) // Prevent division by zero if there are no slaves
      total.cpus = 1;
    if (total.mem == 0)
      total.mem = 1;
  }
  
  bool operator() (Framework *f1, Framework *f2)
  {
    double share1 = max(f1->resources.cpus / (double) total.cpus,
                        f1->resources.mem  / (double) total.mem);
    double share2 = max(f2->resources.cpus / (double) total.cpus,
                        f2->resources.mem  / (double) total.mem);
    if (share1 == share2)
      return f1->id < f2->id; // Make the sort deterministic for unit testing
    else
      return share1 < share2;
  }
};

}


vector<Framework *> SimpleAllocator::getAllocationOrdering()
{
  vector<Framework *> frameworks = master->getActiveFrameworks();
  vector<Slave *> slaves = master->getActiveSlaves();
  Resources total;
  foreach (Slave *slave, slaves) {
    total += slave->resources;
  }
  DominantShareComparator comp(total);
  sort(frameworks.begin(), frameworks.end(), comp);
  return frameworks;
}


void SimpleAllocator::makeNewOffers()
{
  LOG(INFO) << "Running makeNewOffers...";
  
  // Get an ordering of frameworks to send offers to
  vector<Framework *> ordering = getAllocationOrdering();
  if (ordering.size() == 0)
    return;
  
  // Find all the free resources that can be allocated
  unordered_map<Slave *, Resources> freeResources;
  vector<Slave *> slaves = master->getActiveSlaves();
  foreach (Slave *slave, slaves) {
    Resources res = slave->resourcesFree();
    if (res.cpus >= MIN_CPUS && res.mem >= MIN_MEM) {
      LOG(INFO) << "Found free resources: " << res << " on " << slave;
      freeResources[slave] = res;
    }
  }
  if (freeResources.size() == 0)
    return;
  
  // Clear refusers on any slave that has been refused by everyone
  foreachpair (Slave *slave, _, freeResources) {
    unordered_set<Framework *>& refs = refusers[slave];
    if (refs.size() == ordering.size()) {
      LOG(INFO) << "Clearing refusers for " << slave
                << " because everyone refused it";
      refs.clear();
    }
  }
  
  foreach (Framework *framework, ordering) {
    // See which resources this framework can take (given filters & refusals)
    vector<SlaveResources> offerable;
    foreachpair (Slave *slave, Resources resources, freeResources) {
      if (refusers[slave].find(framework) == refusers[slave].end() &&
          !framework->filters(slave, resources)) {
        LOG(INFO) << "Offering " << resources << " on " << slave
                  << " to framework " << framework->id;
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
