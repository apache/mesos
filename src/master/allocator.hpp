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
