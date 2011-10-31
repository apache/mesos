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

#ifndef PROXY_SCHEDULER_HPP
#define PROXY_SCHEDULER_HPP

#ifdef __APPLE__
// Since Python.h defines _XOPEN_SOURCE on Mac OS X, we undefine it
// here so that we don't get warning messages during the build.
#undef _XOPEN_SOURCE
#endif // __APPLE__
#include <Python.h>

#include <string>
#include <vector>

#include <mesos/scheduler.hpp>


namespace mesos { namespace python {

struct MesosSchedulerDriverImpl;

/**
 * Proxy Scheduler implementation that will call into Python
 */
class ProxyScheduler : public Scheduler
{
  MesosSchedulerDriverImpl *impl;

public:
  ProxyScheduler(MesosSchedulerDriverImpl *_impl) : impl(_impl) {}

  virtual ~ProxyScheduler() {}

  // Callbacks for various Mesos events.
  virtual void registered(SchedulerDriver* driver,
                          const FrameworkID& frameworkId);

  virtual void resourceOffers(SchedulerDriver* driver,
                              const std::vector<Offer>& offers);

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId);

  virtual void statusUpdate(SchedulerDriver* driver,
                            const TaskStatus& status);

  virtual void frameworkMessage(SchedulerDriver* driver,
                                const SlaveID& slaveId,
                                const ExecutorID& executorId,
                                const std::string& data);

  virtual void slaveLost(SchedulerDriver* driver,
                         const SlaveID& slaveId);

  virtual void error(SchedulerDriver* driver,
                     int code,
                     const std::string& message);

};

}} /* namespace mesos { namespace python { */

#endif /* PROXY_SCHEDULER_HPP */
