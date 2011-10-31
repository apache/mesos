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

#ifndef __MESOS_SCHEDULER_HPP__
#define __MESOS_SCHEDULER_HPP__

#include <string>
#include <map>
#include <vector>

#include <mesos/mesos.hpp>


namespace mesos {

class SchedulerDriver;

namespace internal {
class SchedulerProcess;
class MasterDetector;
class Configuration;
}


/**
 * Callback interface to be implemented by new frameworks' schedulers.
 */
class Scheduler
{
public:
  virtual ~Scheduler() {}

  // Callbacks for various Mesos events.

  virtual void registered(SchedulerDriver* driver,
                          const FrameworkID& frameworkId) = 0;

  virtual void resourceOffers(SchedulerDriver* driver,
                              const std::vector<Offer>& offers) = 0;

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId) = 0;

  virtual void statusUpdate(SchedulerDriver* driver,
                            const TaskStatus& status) = 0;

  virtual void frameworkMessage(SchedulerDriver* driver,
                                const SlaveID& slaveId,
                                const ExecutorID& executorId,
                                const std::string& data) = 0;

  virtual void slaveLost(SchedulerDriver* driver,
                         const SlaveID& slaveId) = 0;

  virtual void error(SchedulerDriver* driver,
                     int code,
                     const std::string& message) = 0;
};


/**
 * Abstract interface for driving a scheduler connected to Mesos.
 * This interface is used both to manage the scheduler's lifecycle (start it,
 * stop it, or wait for it to finish) and to send commands from the user
 * framework to Mesos (such as replies to offers). Concrete implementations
 * of SchedulerDriver will take a Scheduler as a parameter in order to make
 * callbacks into it on various events.
 */
class SchedulerDriver
{
public:
  virtual ~SchedulerDriver() {}

  // Lifecycle methods.
  virtual Status start() = 0;
  virtual Status stop(bool failover = false) = 0;
  // Puts driver into ABORTED state after which no more callbacks
  // can be made to the scheduler. Also, the master is signalled
  // that the driver is inactived. The only call a scheduler
  // can make after abort is stop(), which can un-register the
  // framework (if requested).
  virtual Status abort() = 0;
  virtual Status join() = 0;
  virtual Status run() = 0; // Start and then join driver.

  // Communication methods.

  virtual Status requestResources(
      const std::vector<ResourceRequest>& requests) = 0;

  virtual Status launchTasks(const OfferID& offerId,
                             const std::vector<TaskDescription>& tasks,
                             const Filters& filters = Filters()) = 0;

  virtual Status killTask(const TaskID& taskId) = 0;

  virtual Status reviveOffers() = 0;

  virtual Status sendFrameworkMessage(const SlaveID& slaveId,
                                      const ExecutorID& executorId,
                                      const std::string& data) = 0;
};


/**
 * Concrete implementation of SchedulerDriver that communicates with
 * a Mesos master.
 */
class MesosSchedulerDriver : public SchedulerDriver
{
public:
  /**
   * Create a scheduler driver with a given Mesos master URL.
   * Additional Mesos config options are read from the environment, as well
   * as any config files found through it.
   *
   * @param sched scheduler to make callbacks into
   * @param url Mesos master URL
   * @param frameworkId optional framework ID for registering
   *        redundant schedulers for the same framework
   */
  MesosSchedulerDriver(Scheduler* sched,
                       const std::string& frameworkName,
                       const ExecutorInfo& executorInfo,
                       const std::string& url,
                       const FrameworkID& frameworkId = FrameworkID());

  /**
   * Create a scheduler driver with a configuration, which the master URL
   * and possibly other options are read from.
   * Additional Mesos config options are read from the environment, as well
   * as any config files given through conf or found in the environment.
   *
   * @param sched scheduler to make callbacks into
   * @param params Map containing configuration options
   * @param frameworkId optional framework ID for registering
   *        redundant schedulers for the same framework
   */
  MesosSchedulerDriver(Scheduler* sched,
                       const std::string& frameworkName,
                       const ExecutorInfo& executorInfo,
                       const std::map<std::string, std::string>& params,
                       const FrameworkID& frameworkId = FrameworkID());

  /**
   * Create a scheduler driver with a config read from command-line arguments.
   * Additional Mesos config options are read from the environment, as well
   * as any config files given through conf or found in the environment.
   *
   * This constructor is not available through SWIG since it's difficult
   * for it to properly map arrays to an argc/argv pair.
   *
   * @param sched scheduler to make callbacks into
   * @param argc argument count
   * @param argv argument values (argument 0 is expected to be program name
   *             and will not be looked at for options)
   * @param frameworkId optional framework ID for registering
   *        redundant schedulers for the same framework
   */
  MesosSchedulerDriver(Scheduler* sched,
                       const std::string& frameworkName,
                       const ExecutorInfo& executorInfo,
                       int argc,
                       char** argv,
                       const FrameworkID& frameworkId = FrameworkID());

  virtual ~MesosSchedulerDriver();

  // Lifecycle methods.
  virtual Status start();
  virtual Status stop(bool failover = false);
  virtual Status abort();
  virtual Status join();
  virtual Status run(); // Start and then join driver.

  // Communication methods.
  virtual Status requestResources(
      const std::vector<ResourceRequest>& requests);

  virtual Status launchTasks(const OfferID& offerId,
                             const std::vector<TaskDescription>& tasks,
                             const Filters& filters = Filters());

  virtual Status killTask(const TaskID& taskId);

  virtual Status reviveOffers();

  virtual Status sendFrameworkMessage(const SlaveID& slaveId,
                                      const ExecutorID& executorId,
                                      const std::string& data);

private:
  // Initialization method used by constructors
  void init(Scheduler* sched,
            internal::Configuration* conf,
            const FrameworkID& frameworkId,
            const std::string& frameworkName,
            const ExecutorInfo& executorInfo);

  // Internal utility method to report an error to the scheduler
  void error(int code, const std::string& message);

  Scheduler* sched;
  std::string url;
  FrameworkID frameworkId;
  std::string frameworkName;
  ExecutorInfo executorInfo;

  // Libprocess process for communicating with master
  internal::SchedulerProcess* process;

  // Coordination between masters
  internal::MasterDetector* detector;

  // Configuration options.
  // TODO(benh|matei): Does this still need to be a pointer?
  internal::Configuration* conf;

  // Mutex to enforce all non-callbacks are execute serially
  pthread_mutex_t mutex;

  // Condition variable for waiting until driver terminates
  pthread_cond_t cond;

  enum State {
    INITIALIZED,
    RUNNING,
    STOPPED,
    ABORTED
  };

  // Variable to store the state of the driver.
  State state;
};


} // namespace mesos {

#endif // __MESOS_SCHEDULER_HPP__
