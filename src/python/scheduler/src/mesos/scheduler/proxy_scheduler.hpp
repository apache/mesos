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

#ifndef PROXY_SCHEDULER_HPP
#define PROXY_SCHEDULER_HPP

// Python.h must be included before standard headers.
// See: http://docs.python.org/2/c-api/intro.html#include-files
#include <Python.h>

#include <string>
#include <vector>

#include <mesos/scheduler.hpp>

namespace mesos {
namespace python {

struct MesosSchedulerDriverImpl;

/**
 * Proxy Scheduler implementation that will call into Python.
 */
class ProxyScheduler : public Scheduler
{
public:
  explicit ProxyScheduler(MesosSchedulerDriverImpl* _impl) : impl(_impl) {}

  virtual ~ProxyScheduler() {}

  virtual void registered(SchedulerDriver* driver,
                          const FrameworkID& frameworkId,
                          const MasterInfo& masterInfo);
  virtual void reregistered(SchedulerDriver* driver,
                            const MasterInfo& masterInfo);
  virtual void disconnected(SchedulerDriver* driver);
  virtual void resourceOffers(SchedulerDriver* driver,
                              const std::vector<Offer>& offers);
  virtual void offerRescinded(SchedulerDriver* driver, const OfferID& offerId);
  virtual void statusUpdate(SchedulerDriver* driver, const TaskStatus& status);
  virtual void frameworkMessage(SchedulerDriver* driver,
                                const ExecutorID& executorId,
                                const SlaveID& slaveId,
                                const std::string& data);
  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& slaveId);
  virtual void executorLost(SchedulerDriver* driver,
                            const ExecutorID& executorId,
                            const SlaveID& slaveId,
                            int status);
  virtual void error(SchedulerDriver* driver, const std::string& message);

private:
  MesosSchedulerDriverImpl* impl;
};

} // namespace python {
} // namespace mesos {

#endif // PROXY_SCHEDULER_HPP
