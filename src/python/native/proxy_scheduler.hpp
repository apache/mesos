#ifndef PROXY_SCHEDULER_HPP
#define PROXY_SCHEDULER_HPP

#include <Python.h>

#include <string>
#include <vector>

#include "mesos_sched.hpp"

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

  // Callbacks for getting framework properties.
  virtual std::string getFrameworkName(SchedulerDriver* driver);

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver* driver);

  // Callbacks for various Mesos events.
  virtual void registered(SchedulerDriver* driver,
                          const FrameworkID& frameworkId);

  virtual void resourceOffer(SchedulerDriver* driver,
                             const OfferID& offerId,
                             const std::vector<SlaveOffer>& offers);

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId);

  virtual void statusUpdate(SchedulerDriver* driver,
                            const TaskStatus& status);

  virtual void frameworkMessage(SchedulerDriver* driver,
                                const FrameworkMessage& message);

  virtual void slaveLost(SchedulerDriver* driver,
                         const SlaveID& slaveId);

  virtual void error(SchedulerDriver* driver,
                     int code,
                     const std::string& message);

};

}} /* namespace mesos { namespace python { */

#endif /* PROXY_SCHEDULER_HPP */
