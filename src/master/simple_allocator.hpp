#ifndef __SIMPLE_ALLOCATOR_HPP__
#define __SIMPLE_ALLOCATOR_HPP__

#include <vector>

#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>

#include "messages/messages.pb.h"

#include "allocator.hpp"


namespace mesos { namespace internal { namespace master {

class SimpleAllocator : public Allocator
{
  Master* master;

  Resources totalResources;
  
  // Remember which frameworks refused each slave "recently"; this is cleared
  // when the slave's free resources go up or when everyone has refused it
  boost::unordered_map<Slave*, boost::unordered_set<Framework*> > refusers;
  
public:
  SimpleAllocator(Master* _master): master(_master) {}
  
  ~SimpleAllocator() {}
  
  virtual void frameworkAdded(Framework* framework);
  
  virtual void frameworkRemoved(Framework* framework);
  
  virtual void slaveAdded(Slave* slave);
  
  virtual void slaveRemoved(Slave* slave);
  
  virtual void taskRemoved(Task* task, TaskRemovalReason reason);

  virtual void offerReturned(Offer* offer,
                             OfferReturnReason reason,
                             const std::vector<SlaveResources>& resourcesLeft);

  virtual void offersRevived(Framework* framework);
  
  virtual void timerTick();
  
private:
  // Get an ordering to consider frameworks in for launching tasks
  std::vector<Framework*> getAllocationOrdering();
  
  // Look at the full state of the cluster and send out offers
  void makeNewOffers();

  // Make resource offers for just one slave
  void makeNewOffers(Slave* slave);

  // Make resource offers for a subset of the slaves
  void makeNewOffers(const std::vector<Slave*>& slaves);
};

}}} /* namespace */

#endif /* __SIMPLE_ALLOCATOR_HPP__ */
