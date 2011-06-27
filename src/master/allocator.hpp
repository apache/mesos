#ifndef __ALLOCATOR_HPP__
#define __ALLOCATOR_HPP__

#include "master/master.hpp"

namespace mesos { namespace internal { namespace master {

class Allocator {
public:
  virtual ~Allocator() {}
  
  virtual void frameworkAdded(Framework *framework) {}
  
  virtual void frameworkRemoved(Framework *framework) {}
  
  virtual void slaveAdded(Slave *slave) {}
  
  virtual void slaveRemoved(Slave *slave) {}
  
  virtual void taskAdded(Task *task) {}
  
  virtual void taskRemoved(Task *task, TaskRemovalReason reason) {}

  virtual void offerReturned(Offer* offer,
                             OfferReturnReason reason,
                             const std::vector<SlaveResources>& resourcesLeft) {}

  virtual void offersRevived(Framework *framework) {}

  virtual void timerTick() {}
};

}}} /* namespace */

#endif /* __ALLOCATOR_HPP__ */
