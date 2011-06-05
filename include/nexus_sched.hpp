#ifndef NEXUS_SCHED_HPP
#define NEXUS_SCHED_HPP

#include <nexus.hpp>

#include <string>
#include <vector>


namespace nexus {

class SchedulerDriver;

namespace internal { class SchedulerProcess; }


/**
 * Callback interface to be implemented by new frameworks' schedulers.
 */
class Scheduler
{
public:
  virtual ~Scheduler() {}

  // Callbacks for getting framework properties
  virtual std::string getFrameworkName(SchedulerDriver*);
  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*);

  // Callbacks for various Nexus events
  virtual void registered(SchedulerDriver* d, FrameworkID fid) {}
  virtual void resourceOffer(SchedulerDriver* d,
                             OfferID oid,
                             const std::vector<SlaveOffer>& offers) {}
  virtual void offerRescinded(SchedulerDriver* d, OfferID oid) {}
  virtual void statusUpdate(SchedulerDriver* d, const TaskStatus& status) {}
  virtual void frameworkMessage(SchedulerDriver* d,
                                const FrameworkMessage& message) {}
  virtual void slaveLost(SchedulerDriver* d, SlaveID sid) {}
  virtual void error(SchedulerDriver* d, int code, const std::string& message);
};


/**
 * Abstract interface for driving a scheduler connected to Nexus.
 * This interface is used both to manage the scheduler's lifecycle (start it,
 * stop it, or wait for it to finish) and to send commands from the user
 * framework to Nexus (such as replies to offers). Concrete implementations
 * of SchedulerDriver will take a Scheduler as a parameter in order to make
 * callbacks into it on various events.
 */
class SchedulerDriver
{
public:
  virtual ~SchedulerDriver() {}

  // Lifecycle methods
  virtual void start() {}
  virtual void join() {}
  virtual void stop() {}
  virtual void run() {} // Start and then join scheduler

  // Communication methods
  virtual void sendFrameworkMessage(const FrameworkMessage& message) {}
  virtual void killTask(TaskID tid) {}
  virtual void replyToOffer(OfferID oid,
                            const std::vector<TaskDescription>& task,
                            const string_map& params) {}
  virtual void reviveOffers() {}
  virtual void sendHints(const string_map& hints) {}
};


/**
 * Concrete implementation of SchedulerDriver that communicates with
 * a Nexus master.
 */
class NexusSchedulerDriver : public SchedulerDriver
{
public:
  NexusSchedulerDriver(Scheduler* sched, const std::string& master);
  virtual ~NexusSchedulerDriver();

  // Lifecycle methods
  virtual void start();
  virtual void join();
  virtual void stop();
  virtual void run(); // Start and then join scheduler

  // Communication methods
  virtual void sendFrameworkMessage(const FrameworkMessage& message);
  virtual void killTask(TaskID tid);
  virtual void replyToOffer(OfferID offerId,
                            const std::vector<TaskDescription>& task,
                            const string_map& params);
  virtual void reviveOffers();
  virtual void sendHints(const string_map& hints);
  
  // Scheduler getter; mostly used in SWIG proxies
  virtual Scheduler* getScheduler() { return sched; }

private:
  void error(int code, const std::string& message);

  std::string master;
  Scheduler* sched;

  // LibProcess process for communicating with master
  internal::SchedulerProcess* process;

  // Are we currently registered with the master
  bool running;
  
  // Mutex to enforce all non-callbacks are execute serially
  pthread_mutex_t mutex;

  // Condition variable for waiting until scheduler terminates
  pthread_cond_t cond;
};


} /* namespace nexus { */

#endif /* NEXUS_SCHED_HPP */
