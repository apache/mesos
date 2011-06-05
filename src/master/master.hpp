#ifndef __MASTER_HPP__
#define __MASTER_HPP__

#include <time.h>
#include <arpa/inet.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <process.hpp>

#include <glog/logging.h>

#include <boost/lexical_cast.hpp>
#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>

#include "state.hpp"

#include "common/build.hpp"
#include "common/fatal.hpp"
#include "common/foreach.hpp"
#include "common/resources.hpp"
#include "common/type_utils.hpp"

#include "configurator/configurator.hpp"

#include "detector/detector.hpp"

#include "messaging/messages.hpp"


namespace mesos { namespace internal { namespace master {

using namespace mesos;
using namespace mesos::internal;

using boost::unordered_map;
using boost::unordered_set;

using foreach::_;

using std::make_pair;
using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;


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

// Interval that slaves should send heartbeats.
const double HEARTBEAT_INTERVAL = 2;

// Acceptable time since we saw the last heartbeat (four heartbeats).
const double HEARTBEAT_TIMEOUT = 15;

// Time to wait for a framework to failover (TODO(benh): Make configurable)).
const time_t FRAMEWORK_FAILOVER_TIMEOUT = 60 * 60 * 24;

// Some forward declarations
struct Slave;
class Allocator;


class FrameworkFailoverTimer : public MesosProcess
{
public:
  FrameworkFailoverTimer(const PID &_master, const FrameworkID& _frameworkId)
    : master(_master), frameworkId(_frameworkId) {}

protected:
  virtual void operator () ()
  {
    link(master);
    do {
      switch (receive(FRAMEWORK_FAILOVER_TIMEOUT)) {
        case PROCESS_TIMEOUT: {
          MSG<M2M_FRAMEWORK_EXPIRED> msg;
          msg.mutable_framework_id()->set_value(frameworkId.value());
          send(master, msg);
          return;
        }
        case PROCESS_EXIT:
        case M2M_SHUTDOWN:
          return;
      }
    } while (true);
  }

private:
  const PID master;
  const FrameworkID frameworkId;
};


// Resources offered on a particular slave.
struct SlaveResources
{
  Slave *slave;
  Resources resources;
  
  SlaveResources() {}
  
  SlaveResources(Slave *s, Resources r): slave(s), resources(r) {}
};


// A resource offer.
struct SlotOffer
{
  OfferID offerId;
  FrameworkID frameworkId;
  vector<SlaveResources> resources;

  SlotOffer(const OfferID& _offerId,
            const FrameworkID& _frameworkId,
            const vector<SlaveResources>& _resources)
    : offerId(_offerId), frameworkId(_frameworkId), resources(_resources) {}
};


// An connected framework.
struct Framework
{
  FrameworkInfo info;
  FrameworkID frameworkId;
  PID pid;

  bool active; // Turns false when framework is being removed
  double connectTime;

  unordered_map<TaskID, Task*> tasks;
  unordered_set<SlotOffer*> slotOffers; // Active offers for framework.

  Resources resources; // Total resources owned by framework (tasks + offers)
  
  // Contains a time of unfiltering for each slave we've filtered,
  // or 0 for slaves that we want to keep filtered forever
  unordered_map<Slave *, double> slaveFilter;

  // A failover timer if the connection to this framework is lost.
  FrameworkFailoverTimer *failoverTimer;

  Framework(const FrameworkInfo& _info, const FrameworkID& _frameworkId,
            const PID& _pid, double time)
    : info(_info), frameworkId(_frameworkId), pid(_pid), active(true),
      connectTime(time), failoverTimer(NULL) {}

  ~Framework()
  {
    if (failoverTimer != NULL) {
      MesosProcess::post(failoverTimer->self(), M2M_SHUTDOWN);
      Process::wait(failoverTimer->self());
      delete failoverTimer;
      failoverTimer = NULL;
    }
  }
  
  Task * lookupTask(const TaskID& taskId)
  {
    unordered_map<TaskID, Task *>::iterator it = tasks.find(taskId);
    if (it != tasks.end())
      return it->second;
    else
      return NULL;
  }
  
  void addTask(Task *task)
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
  
  void addOffer(SlotOffer *offer)
  {
    CHECK(slotOffers.count(offer) == 0);
    slotOffers.insert(offer);
    foreach (const SlaveResources& sr, offer->resources) {
      resources += sr.resources;
    }
  }

  void removeOffer(SlotOffer *offer)
  {
    CHECK(slotOffers.find(offer) != slotOffers.end());
    slotOffers.erase(offer);
    foreach (const SlaveResources& sr, offer->resources) {
      resources -= sr.resources;
    }
  }
  
  bool filters(Slave *slave, Resources resources)
  {
    // TODO: Implement other filters
    return slaveFilter.find(slave) != slaveFilter.end();
  }
  
  void removeExpiredFilters(double now)
  {
    vector<Slave *> toRemove;
    foreachpair (Slave *slave, double removalTime, slaveFilter)
      if (removalTime != 0 && removalTime <= now)
        toRemove.push_back(slave);
    foreach (Slave *slave, toRemove)
      slaveFilter.erase(slave);
  }
};


// A connected slave.
struct Slave
{
  SlaveInfo info;
  SlaveID slaveId;
  PID pid;

  bool active; // Turns false when slave is being removed
  double connectTime;
  double lastHeartbeat;
  
  Resources resourcesOffered; // Resources currently in offers
  Resources resourcesInUse;   // Resources currently used by tasks

  unordered_map<pair<FrameworkID, TaskID>, Task *> tasks;
  unordered_set<SlotOffer *> slotOffers; // Active offers on this slave.
  
  Slave(const SlaveInfo& _info, const SlaveID& _slaveId,
        const PID& _pid, double time)
    : info(_info), slaveId(_slaveId), pid(_pid), active(true),
      connectTime(time), lastHeartbeat(time) {}

  ~Slave() {}

  Task * lookupTask(const FrameworkID& frameworkId, const TaskID& taskId)
  {
    foreachpair (_, Task *task, tasks)
      if (task->framework_id() == frameworkId && task->task_id() == taskId)
        return task;

    return NULL;
  }

  void addTask(Task *task)
  {
    CHECK(tasks.count(make_pair(task->framework_id(), task->task_id())) == 0);
    tasks[make_pair(task->framework_id(), task->task_id())] = task;
    foreach (const Resource& resource, task->resources()) {
      resourcesInUse += resource;
    }
  }
  
  void removeTask(Task *task)
  {
    CHECK(tasks.count(make_pair(task->framework_id(), task->task_id())) > 0);
    tasks.erase(make_pair(task->framework_id(), task->task_id()));
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


class Master : public MesosProcess
{
public:
  Master();

  Master(const Configuration& conf);
  
  ~Master();

  static void registerOptions(Configurator* conf);

  state::MasterState *getState();
  
  OfferID makeOffer(Framework *framework,
		    const vector<SlaveResources>& resources);
  
  void rescindOffer(SlotOffer *offer);
  
  void killTask(Task *task);
  
  Framework * lookupFramework(const FrameworkID& frameworkId);

  Slave * lookupSlave(const SlaveID& slaveId);

  SlotOffer * lookupSlotOffer(const OfferID& offerId);

  // Return connected frameworks that are not in the process of being removed
  vector<Framework *> getActiveFrameworks();
  
  // Return connected slaves that are not in the process of being removed
  vector<Slave *> getActiveSlaves();

  const Configuration& getConfiguration();

protected:
  virtual void operator () ();

  // Process a resource offer reply (for a non-cancelled offer) by launching
  // the desired tasks (if the offer contains a valid set of tasks) and
  // reporting any unused resources to the allocator
  void processOfferReply(SlotOffer *offer,
                         const vector<TaskDescription>& tasks,
                         const Params& params);

  // Launch a task described in a slot offer response
  void launchTask(Framework *framework, const TaskDescription& task);
  
  // Terminate a framework, sending it a particular error message
  // TODO: Make the error codes and messages programmer-friendly
  void terminateFramework(Framework *framework,
                          int32_t code,
                          const std::string& message);
  
  // Remove a slot offer (because it was replied to, or we want to rescind it,
  // or we lost a framework or a slave)
  void removeSlotOffer(SlotOffer *offer,
                       OfferReturnReason reason,
                       const vector<SlaveResources>& resourcesLeft);

  void removeTask(Task *task, TaskRemovalReason reason);

  void addFramework(Framework *framework);

  // Replace the scheduler for a framework with a new process ID, in the
  // event of a scheduler failover.
  void failoverFramework(Framework *framework, const PID &newPid);

  // Kill all of a framework's tasks, delete the framework object, and
  // reschedule slot offers for slots that were assigned to this framework
  void removeFramework(Framework *framework);

  // Lose all of a slave's tasks and delete the slave object
  void removeSlave(Slave *slave);

  virtual Allocator* createAllocator();

  FrameworkID newFrameworkId();
  OfferID newOfferId();
  SlaveID newSlaveId();

private:
  Configuration conf;

  unordered_map<FrameworkID, Framework *> frameworks;
  unordered_map<SlaveID, Slave *> slaves;
  unordered_map<OfferID, SlotOffer *> slotOffers;

  unordered_map<PID, FrameworkID> pidToFrameworkId;
  unordered_map<PID, SlaveID> pidToSlaveId;

  int64_t nextFrameworkId; // Used to give each framework a unique ID.
  int64_t nextOfferId;     // Used to give each slot offer a unique ID.
  int64_t nextSlaveId;     // Used to give each slave a unique ID.

  string allocatorType;
  Allocator *allocator;

  bool active;

  string masterId; // Contains the date the master was launched and
                   // some ephemeral token (e.g. returned from
                   // ZooKeeper). Used in framework and slave IDs
                   // created by this master.
};


// Pretty-printing of SlotOffers, Tasks, Frameworks, Slaves, etc

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


}}} /* namespace */

#endif /* __MASTER_HPP__ */
