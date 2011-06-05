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

  // Callbacks for getting framework properties.
  virtual std::string getFrameworkName(SchedulerDriver* driver) = 0;

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver* driver) = 0;

  // Callbacks for various Mesos events.

  virtual void registered(SchedulerDriver* driver,
                          const FrameworkID& frameworkId) = 0;

  virtual void resourceOffer(SchedulerDriver* driver,
                             const OfferID& offerId,
                             const std::vector<SlaveOffer>& offers) = 0;

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
  virtual int start() = 0;
  virtual int stop() = 0;
  virtual int join() = 0;
  virtual int run() = 0; // Start and then join driver.

  // Communication methods.

  virtual int sendFrameworkMessage(const SlaveID& slaveId,
				   const ExecutorID& executorId,
				   const std::string& data) = 0;

  virtual int killTask(const TaskID& taskId) = 0;

  virtual int replyToOffer(const OfferID& offerId,
                           const std::vector<TaskDescription>& tasks,
                           const std::map<std::string, std::string>& params) = 0;

  virtual int replyToOffer(const OfferID& offerId,
                           const std::vector<TaskDescription>& tasks)
  {
    return replyToOffer(offerId, tasks, std::map<std::string, std::string>());
  }

  virtual int reviveOffers() = 0;
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
                       const std::map<std::string, std::string>& params,
                       const FrameworkID& frameworkId = FrameworkID());

#ifndef SWIG
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
                       int argc,
                       char** argv,
                       const FrameworkID& frameworkId = FrameworkID());
#endif

  virtual ~MesosSchedulerDriver();

  // Lifecycle methods.
  virtual int start();
  virtual int stop();
  virtual int join();
  virtual int run(); // Start and then join driver.

  // Communication methods.
  virtual int sendFrameworkMessage(const SlaveID& slaveId,
				   const ExecutorID& executorId,
				   const std::string& data);

  virtual int killTask(const TaskID& taskId);

  virtual int replyToOffer(const OfferID& offerId,
                           const std::vector<TaskDescription>& tasks,
                           const std::map<std::string, std::string>& params);

  virtual int replyToOffer(const OfferID& offerId,
                           const std::vector<TaskDescription>& tasks)
  {
    return replyToOffer(offerId, tasks, std::map<std::string, std::string>());
  }

  virtual int reviveOffers();

private:
  // Initialization method used by constructors
  void init(Scheduler* sched,
            internal::Configuration* conf,
            const FrameworkID& frameworkId);

  // Internal utility method to report an error to the scheduler
  void error(int code, const std::string& message);

  Scheduler* sched;
  std::string url;
  FrameworkID frameworkId;

  // Libprocess process for communicating with master
  internal::SchedulerProcess* process;

  // Coordination between masters
  internal::MasterDetector* detector;

  // Configuration options.
  // TODO(benh|matei): Does this still need to be a pointer?
  internal::Configuration* conf;

  // Are we currently registered with the master
  bool running;
  
  // Mutex to enforce all non-callbacks are execute serially
  pthread_mutex_t mutex;

  // Condition variable for waiting until driver terminates
  pthread_cond_t cond;
};


} // namespace mesos {

#endif // __MESOS_SCHEDULER_HPP__
