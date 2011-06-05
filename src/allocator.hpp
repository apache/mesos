#ifndef __ALLOCATOR_HPP__
#define __ALLOCATOR_HPP__

#include "master.hpp"

namespace nexus { namespace internal { namespace master {

using std::vector;

class Allocator {
public:
  virtual ~Allocator() {}
  
  virtual void frameworkAdded(Framework *framework) {}
  
  virtual void frameworkRemoved(Framework *framework) {}
  
  virtual void slaveAdded(Slave *slave) {}
  
  virtual void slaveRemoved(Slave *slave) {}
  
  virtual void taskAdded(Task *task) {}
  
  virtual void taskRemoved(Task *task, TaskRemovalReason reason) {}

  virtual void offerReturned(SlotOffer* offer,
                             OfferReturnReason reason,
                             const vector<SlaveResources>& resourcesLeft) {}

  virtual void offersRevived(Framework *framework) {}

  virtual void timerTick() {}
};

}}} /* namespace */

#endif /* __ALLOCATOR_HPP__ */
