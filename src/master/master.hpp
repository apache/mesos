#ifndef __MASTER_HPP__
#define __MASTER_HPP__

#include <string>
#include <vector>

#include <process.hpp>

#include <glog/logging.h>

#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>

#include "state.hpp"

#include "common/foreach.hpp"
#include "common/multimap.hpp"
#include "common/resources.hpp"
#include "common/type_utils.hpp"

#include "configurator/configurator.hpp"

#include "messaging/messages.hpp"


namespace mesos { namespace internal { namespace master {

using foreach::_;


// Maximum number of slot offers to have outstanding for each framework.
const int MAX_OFFERS_PER_FRAMEWORK = 50;

// Default number of seconds until a refused slot is resent to a framework.
const double DEFAULT_REFUSAL_TIMEOUT = 5;

// Minimum number of cpus / task.
const int32_t MIN_CPUS = 1;

// Minimum amount of memory / task.
const int32_t MIN_MEM = 32 * Megabyte;

// Maximum number of CPUs per machine.
const int32_t MAX_CPUS = 1000 * 1000;

// Maximum amount of memory / machine.
const int32_t MAX_MEM = 1024 * 1024 * Megabyte;

// Acceptable timeout for slave PONG.
const double SLAVE_PONG_TIMEOUT = 15.0;

// Maximum number of timeouts until slave is considered failed.
const int MAX_SLAVE_TIMEOUTS = 5;

// Time to wait for a framework to failover (TODO(benh): Make configurable)).
const time_t FRAMEWORK_FAILOVER_TIMEOUT = 60 * 60 * 24;


// Reasons why offers might be returned to the Allocator.
enum OfferReturnReason
{
  ORR_FRAMEWORK_REPLIED,
  ORR_OFFER_RESCINDED,
  ORR_FRAMEWORK_LOST,
  ORR_FRAMEWORK_FAILOVER,
  ORR_SLAVE_LOST
};


// Reasons why tasks might be removed, passed to the Allocator.
enum TaskRemovalReason
{
  TRR_TASK_ENDED,
  TRR_FRAMEWORK_LOST,
  TRR_EXECUTOR_LOST,
  TRR_SLAVE_LOST
};


// Some forward declarations.
class Allocator;
class SlavesManager;
struct Framework;
struct Slave;
struct SlaveResources;
class SlaveObserver;
struct SlotOffer;


class Master : public MesosProcess<Master>
{
public:
  Master();
  Master(const Configuration& conf);
  
  virtual ~Master();

  static void registerOptions(Configurator* configurator);

  process::Promise<state::MasterState*> getState();
  
  OfferID makeOffer(Framework* framework,
		    const std::vector<SlaveResources>& resources);

  // Return connected frameworks that are not in the process of being removed
  std::vector<Framework*> getActiveFrameworks();
  
  // Return connected slaves that are not in the process of being removed
  std::vector<Slave*> getActiveSlaves();

  void newMasterDetected(const std::string& pid);
  void noMasterDetected();
  void masterDetectionFailure();
  void registerFramework(const FrameworkInfo& frameworkInfo);
  void reregisterFramework(const FrameworkID& frameworkId,
                           const FrameworkInfo& frameworkInfo,
                           int32_t generation);
  void unregisterFramework(const FrameworkID& frameworkId);
  void resourceOfferReply(const FrameworkID& frameworkId,
                          const OfferID& offerId,
                          const std::vector<TaskDescription>& tasks,
                          const Params& params);
  void reviveOffers(const FrameworkID& frameworkId);
  void killTask(const FrameworkID& frameworkId,
                const TaskID& taskId);
  void schedulerMessage(const SlaveID& slaveId,
			const FrameworkID& frameworkId,
			const ExecutorID& executorId,
			const std::string& data);
  void statusUpdateAck(const FrameworkID& frameworkId,
                       const TaskID& taskId,
                       const SlaveID& slaveId);
  void registerSlave(const SlaveInfo& slaveInfo);
  void reregisterSlave(const SlaveID& slaveId,
                       const SlaveInfo& slaveInfo,
                       const std::vector<Task>& tasks);
  void unregisterSlave(const SlaveID& slaveId);
  void statusUpdate(const FrameworkID& frameworkId,
                    const TaskStatus& status);
  void executorMessage(const SlaveID& slaveId,
		       const FrameworkID& frameworkId,
		       const ExecutorID& executorId,
		       const std::string& data);
  void exitedExecutor(const SlaveID& slaveId,
                      const FrameworkID& frameworkId,
                      const ExecutorID& executorId,
                      int32_t result);
  void activatedSlaveHostnamePort(const std::string& hostname, uint16_t port);
  void deactivatedSlaveHostnamePort(const std::string& hostname, uint16_t port);
  void timerTick();
  void frameworkExpired(const FrameworkID& frameworkId);
  void exited();

  process::Promise<process::HttpResponse> vars(const process::HttpRequest& request);
  process::Promise<process::HttpResponse> stats(const process::HttpRequest& request);

  Framework* lookupFramework(const FrameworkID& frameworkId);
  Slave* lookupSlave(const SlaveID& slaveId);
  SlotOffer* lookupSlotOffer(const OfferID& offerId);
  
protected:
  virtual void operator () ();
  
  void initialize();

  // Process a resource offer reply (for a non-cancelled offer) by launching
  // the desired tasks (if the offer contains a valid set of tasks) and
  // reporting any unused resources to the allocator
  void processOfferReply(SlotOffer* offer,
                         const std::vector<TaskDescription>& tasks,
                         const Params& params);

  // Launch a task described in a slot offer response
  void launchTask(Framework* framework, const TaskDescription& task);
  
  // Terminate a framework, sending it a particular error message
  // TODO: Make the error codes and messages programmer-friendly
  void terminateFramework(Framework* framework,
                          int32_t code,
                          const std::string& message);
  
  // Remove a slot offer (because it was replied to, or we want to rescind it,
  // or we lost a framework or a slave)
  void removeSlotOffer(SlotOffer* offer,
                       OfferReturnReason reason,
                       const std::vector<SlaveResources>& resourcesLeft);

  void removeTask(Task* task, TaskRemovalReason reason);

  void addFramework(Framework* framework);

  // Replace the scheduler for a framework with a new process ID, in the
  // event of a scheduler failover.
  void failoverFramework(Framework* framework, const process::UPID& newPid);

  // Kill all of a framework's tasks, delete the framework object, and
  // reschedule slot offers for slots that were assigned to this framework
  void removeFramework(Framework* framework);

  // Add a slave.
  void addSlave(Slave* slave);

  void readdSlave(Slave* slave, const std::vector<Task>& tasks);

  // Lose all of a slave's tasks and delete the slave object
  void removeSlave(Slave* slave);

  virtual Allocator* createAllocator();

  FrameworkID newFrameworkId();
  OfferID newOfferId();
  SlaveID newSlaveId();

  const Configuration& getConfiguration();

private:
  const Configuration conf;

  SlavesManager* slavesManager;

  multimap<std::string, uint16_t> slaveHostnamePorts;

  boost::unordered_map<FrameworkID, Framework*> frameworks;
  boost::unordered_map<SlaveID, Slave*> slaves;
  boost::unordered_map<OfferID, SlotOffer*> slotOffers;

  boost::unordered_map<process::UPID, FrameworkID> pidToFrameworkId;
  boost::unordered_map<process::UPID, SlaveID> pidToSlaveId;

  int64_t nextFrameworkId; // Used to give each framework a unique ID.
  int64_t nextOfferId;     // Used to give each slot offer a unique ID.
  int64_t nextSlaveId;     // Used to give each slave a unique ID.

  std::string allocatorType;
  Allocator* allocator;

  bool active;

  // Contains the date the master was launched and
  // some ephemeral token (e.g. returned from
  // ZooKeeper). Used in framework and slave IDs
  // created by this master.
  std::string masterId;

  // Statistics!
  struct Statistics {
    uint64_t launched_tasks;
    uint64_t finished_tasks;
    uint64_t killed_tasks;
    uint64_t failed_tasks;
    uint64_t lost_tasks;
    uint64_t valid_status_updates;
    uint64_t invalid_status_updates;
    uint64_t valid_framework_messages;
    uint64_t invalid_framework_messages;
  } statistics;
};


// A resource offer.
struct SlotOffer
{
  OfferID offerId;
  FrameworkID frameworkId;
  std::vector<SlaveResources> resources;

  SlotOffer(const OfferID& _offerId,
            const FrameworkID& _frameworkId,
            const std::vector<SlaveResources>& _resources)
    : offerId(_offerId), frameworkId(_frameworkId), resources(_resources) {}
};


// A connected slave.
struct Slave
{
  SlaveInfo info;
  SlaveID slaveId;
  process::UPID pid;

  bool active; // Turns false when slave is being removed
  double connectTime;
  double lastHeartbeat;
  
  Resources resourcesOffered; // Resources currently in offers
  Resources resourcesInUse;   // Resources currently used by tasks

  boost::unordered_map<std::pair<FrameworkID, TaskID>, Task*> tasks;
  boost::unordered_set<SlotOffer*> slotOffers; // Active offers on this slave.

  SlaveObserver* observer;
  
  Slave(const SlaveInfo& _info, const SlaveID& _slaveId,
        const process::UPID& _pid, double time)
    : info(_info), slaveId(_slaveId), pid(_pid), active(true),
      connectTime(time), lastHeartbeat(time) {}

  ~Slave() {}

  Task* lookupTask(const FrameworkID& frameworkId, const TaskID& taskId)
  {
    foreachpair (_, Task* task, tasks) {
      if (task->framework_id() == frameworkId && task->task_id() == taskId) {
        return task;
      }
    }

    return NULL;
  }

  void addTask(Task* task)
  {
    std::pair<FrameworkID, TaskID> key =
      std::make_pair(task->framework_id(), task->task_id());
    CHECK(tasks.count(key) == 0);
    tasks[key] = task;
    foreach (const Resource& resource, task->resources()) {
      resourcesInUse += resource;
    }
  }
  
  void removeTask(Task* task)
  {
    std::pair<FrameworkID, TaskID> key =
      std::make_pair(task->framework_id(), task->task_id());
    CHECK(tasks.count(key) > 0);
    tasks.erase(key);
    foreach (const Resource& resource, task->resources()) {
      resourcesInUse -= resource;
    }
  }
  
  Resources resourcesFree()
  {
    Resources resources;
    foreach (const Resource& resource, info.resources()) {
      resources += resource;
    }
    return resources - (resourcesOffered + resourcesInUse);
  }
};


// Resources offered on a particular slave.
struct SlaveResources
{
  SlaveResources() {}
  SlaveResources(Slave* s, Resources r): slave(s), resources(r) {}

  Slave* slave;
  Resources resources;
};


class FrameworkFailoverTimer : public process::Process<FrameworkFailoverTimer>
{
public:
  FrameworkFailoverTimer(const process::PID<Master>& _master,
                         const FrameworkID& _frameworkId)
    : master(_master), frameworkId(_frameworkId) {}

protected:
  virtual void operator () ()
  {
    link(master);
    while (true) {
      receive(FRAMEWORK_FAILOVER_TIMEOUT);
      if (name() == process::TIMEOUT) {
        process::dispatch(master, &Master::frameworkExpired, frameworkId);
        return;
      } else if (name() == process::EXITED || name() == process::TERMINATE) {
        return;
      }
    }
  }

private:
  const process::PID<Master> master;
  const FrameworkID frameworkId;
};


// An connected framework.
struct Framework
{
  FrameworkInfo info;
  FrameworkID frameworkId;
  process::UPID pid;

  bool active; // Turns false when framework is being removed
  double connectTime;

  boost::unordered_map<TaskID, Task*> tasks;
  boost::unordered_set<SlotOffer*> slotOffers; // Active offers for framework.

  Resources resources; // Total resources owned by framework (tasks + offers)
  
  // Contains a time of unfiltering for each slave we've filtered,
  // or 0 for slaves that we want to keep filtered forever
  boost::unordered_map<Slave*, double> slaveFilter;

  // A failover timer if the connection to this framework is lost.
  FrameworkFailoverTimer* failoverTimer;

  Framework(const FrameworkInfo& _info, const FrameworkID& _frameworkId,
            const process::UPID& _pid, double time)
    : info(_info), frameworkId(_frameworkId), pid(_pid), active(true),
      connectTime(time), failoverTimer(NULL) {}

  ~Framework()
  {
    if (failoverTimer != NULL) {
      process::post(failoverTimer->self(), process::TERMINATE);
      process::wait(failoverTimer->self());
      delete failoverTimer;
    }
  }
  
  Task* lookupTask(const TaskID& taskId)
  {
    if (tasks.count(taskId) > 0) {
      return tasks[taskId];
    } else {
      return NULL;
    }
  }
  
  void addTask(Task* task)
  {
    CHECK(tasks.count(task->task_id()) == 0);
    tasks[task->task_id()] = task;
    for (int i = 0; i < task->resources_size(); i++) {
      resources += task->resources(i);
    }
  }
  
  void removeTask(const TaskID& taskId)
  {
    CHECK(tasks.count(taskId) > 0);
    Task* task = tasks[taskId];
    for (int i = 0; i < task->resources_size(); i++) {
      resources -= task->resources(i);
    }
    tasks.erase(taskId);
  }
  
  void addOffer(SlotOffer* offer)
  {
    CHECK(slotOffers.count(offer) == 0);
    slotOffers.insert(offer);
    foreach (const SlaveResources& sr, offer->resources) {
      resources += sr.resources;
    }
  }

  void removeOffer(SlotOffer* offer)
  {
    CHECK(slotOffers.find(offer) != slotOffers.end());
    slotOffers.erase(offer);
    foreach (const SlaveResources& sr, offer->resources) {
      resources -= sr.resources;
    }
  }
  
  bool filters(Slave* slave, Resources resources)
  {
    // TODO: Implement other filters
    return slaveFilter.find(slave) != slaveFilter.end();
  }
  
  void removeExpiredFilters(double now)
  {
    foreachpaircopy (Slave* slave, double removalTime, slaveFilter) {
      if (removalTime != 0 && removalTime <= now) {
        slaveFilter.erase(slave);
      }
    }
  }
};


// Pretty-printing of SlotOffers, Tasks, Frameworks, Slaves, etc.

inline std::ostream& operator << (std::ostream& stream, const SlotOffer *o)
{
  stream << "offer " << o->offerId;
  return stream;
}


inline std::ostream& operator << (std::ostream& stream, const Slave *s)
{
  stream << "slave " << s->slaveId;
  return stream;
}


inline std::ostream& operator << (std::ostream& stream, const Framework *f)
{
  stream << "framework " << f->frameworkId;
  return stream;
}

}}} // namespace mesos { namespace internal { namespace master {

#endif // __MASTER_HPP__
