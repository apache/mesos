#ifndef __MASTER_HPP__
#define __MASTER_HPP__

#include <time.h>
#include <arpa/inet.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <reliable.hpp>

#include <glog/logging.h>

#include <boost/lexical_cast.hpp>
#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>

#include "state.hpp"

#include "common/fatal.hpp"
#include "common/foreach.hpp"
#include "common/params.hpp"
#include "common/resources.hpp"
#include "common/task.hpp"

#include "configurator/configurator.hpp"

#include "detector/detector.hpp"

#include "event_history/event_logger.hpp"

#include "messaging/messages.hpp"


namespace mesos { namespace internal { namespace master {

using std::make_pair;
using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;

using boost::unordered_map;
using boost::unordered_set;

using namespace mesos;
using namespace mesos::internal;
using mesos::internal::eventhistory::EventLogger;


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
const time_t FRAMEWORK_FAILOVER_TIMEOUT = 60;

// Some forward declarations
struct Slave;
class Allocator;


class FrameworkFailoverTimer : public MesosProcess
{
private:
  const PID master;
  const FrameworkID fid;

protected:
  void operator () ()
  {
    link(master);
    do {
      switch (receive(FRAMEWORK_FAILOVER_TIMEOUT)) {
      case PROCESS_TIMEOUT:
        send(master, pack<M2M_FRAMEWORK_EXPIRED>(fid));
	return;
      case PROCESS_EXIT:
	return;
      case M2M_SHUTDOWN:
	return;
      }
    } while (true);
  }

public:
  FrameworkFailoverTimer(const PID &_master, FrameworkID _fid)
    : master(_master), fid(_fid) {}
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
  OfferID id;
  FrameworkID frameworkId;
  vector<SlaveResources> resources;
  
  SlotOffer(OfferID i, FrameworkID f, const vector<SlaveResources>& r)
    : id(i), frameworkId(f), resources(r) {}
};

// An connected framework.
struct Framework
{
  PID pid;
  FrameworkID id;
  bool active; // Turns false when framework is being removed
  string name;
  string user;
  ExecutorInfo executorInfo;
  double connectTime;

  unordered_map<TaskID, Task *> tasks;
  unordered_set<SlotOffer *> slotOffers; // Active offers given to this framework

  Resources resources; // Total resources owned by framework (tasks + offers)
  
  // Contains a time of unfiltering for each slave we've filtered,
  // or 0 for slaves that we want to keep filtered forever
  unordered_map<Slave *, double> slaveFilter;

  // A failover timer if the connection to this framework is lost.
  FrameworkFailoverTimer *failoverTimer;

  Framework(const PID &_pid, FrameworkID _id, double time)
    : pid(_pid), id(_id), active(true), connectTime(time),
      failoverTimer(NULL) {}

  ~Framework()
  {
    if (failoverTimer != NULL) {
      MesosProcess::post(failoverTimer->self(), pack<M2M_SHUTDOWN>());
      Process::wait(failoverTimer->self());
      delete failoverTimer;
      failoverTimer = NULL;
    }
  }
  
  Task * lookupTask(TaskID tid)
  {
    unordered_map<TaskID, Task *>::iterator it = tasks.find(tid);
    if (it != tasks.end())
      return it->second;
    else
      return NULL;
  }
  
  void addTask(Task *task)
  {
    CHECK(tasks.count(task->id) == 0);
    tasks[task->id] = task;
    this->resources += task->resources;
  }
  
  void removeTask(TaskID tid)
  {
    CHECK(tasks.find(tid) != tasks.end());
    unordered_map<TaskID, Task *>::iterator it = tasks.find(tid);
    this->resources -= it->second->resources;
    tasks.erase(it);
  }
  
  void addOffer(SlotOffer *offer)
  {
    CHECK(slotOffers.find(offer) == slotOffers.end());
    slotOffers.insert(offer);
    foreach (SlaveResources &r, offer->resources)
      this->resources += r.resources;
  }

  void removeOffer(SlotOffer *offer)
  {
    CHECK(slotOffers.find(offer) != slotOffers.end());
    slotOffers.erase(offer);
    foreach (SlaveResources &r, offer->resources)
      this->resources -= r.resources;
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
  PID pid;
  SlaveID id;
  bool active; // Turns false when slave is being removed
  string hostname;
  string webUIUrl;
  double connectTime;
  double lastHeartbeat;
  
  Resources resources;        // Total resources on slave
  Resources resourcesOffered; // Resources currently in offers
  Resources resourcesInUse;   // Resources currently used by tasks

  unordered_map<pair<FrameworkID, TaskID>, Task *> tasks;
  unordered_set<SlotOffer *> slotOffers; // Active offers of slots on this slave
  
  Slave(const PID &_pid, SlaveID _id, double time)
    : pid(_pid), id(_id), active(true)
  {
    connectTime = lastHeartbeat = time;
  }

  Task * lookupTask(FrameworkID fid, TaskID tid)
  {
    foreachpair (_, Task *task, tasks)
      if (task->frameworkId == fid && task->id == tid)
        return task;

    return NULL;
  }

  void addTask(Task *task)
  {
    CHECK(tasks.find(make_pair(task->frameworkId, task->id)) == tasks.end());
    tasks[make_pair(task->frameworkId, task->id)] = task;
    resourcesInUse += task->resources;
  }
  
  void removeTask(Task *task)
  {
    CHECK(tasks.find(make_pair(task->frameworkId, task->id)) != tasks.end());
    tasks.erase(make_pair(task->frameworkId, task->id));
    resourcesInUse -= task->resources;
  }
  
  Resources resourcesFree()
  {
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
protected:
  Params conf;
  EventLogger* evLogger;

  unordered_map<FrameworkID, Framework *> frameworks;
  unordered_map<SlaveID, Slave *> slaves;
  unordered_map<OfferID, SlotOffer *> slotOffers;

  unordered_map<PID, FrameworkID> pidToFid;
  unordered_map<PID, SlaveID> pidToSid;

  int64_t nextFrameworkId; // Used to give each framework a unique ID.
  int64_t nextSlaveId;     // Used to give each slave a unique ID.
  int64_t nextSlotOfferId; // Used to give each slot offer a unique ID.

  string allocatorType;
  Allocator *allocator;

  int64_t masterId; // Used to differentiate masters in fault tolerant mode;
                    // will be this master's ZooKeeper ephemeral id

public:
  Master(EventLogger* evLogger);

  Master(const Params& conf, EventLogger* evLogger);
  
  ~Master();

  static void registerOptions(Configurator* conf);

  state::MasterState *getState();
  
  OfferID makeOffer(Framework *framework,
		    const vector<SlaveResources>& resources);
  
  void rescindOffer(SlotOffer *offer);
  
  void killTask(Task *task);
  
  Framework * lookupFramework(FrameworkID fid);

  Slave * lookupSlave(SlaveID sid);

  SlotOffer * lookupSlotOffer(OfferID soid);

  // Return connected frameworks that are not in the process of being removed
  vector<Framework *> getActiveFrameworks();
  
  // Return connected slaves that are not in the process of being removed
  vector<Slave *> getActiveSlaves();

  const Params& getConf();

protected:
  void operator () ();

  // Process a resource offer reply (for a non-cancelled offer) by launching
  // the desired tasks (if the offer contains a valid set of tasks) and
  // reporting any unused resources to the allocator
  void processOfferReply(SlotOffer *offer,
      const vector<TaskDescription>& tasks, const Params& params);

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

  void replaceFramework(Framework *old, Framework *current);

  // Kill all of a framework's tasks, delete the framework object, and
  // reschedule slot offers for slots that were assigned to this framework
  void removeFramework(Framework *framework);

  // Lose all of a slave's tasks and delete the slave object
  void removeSlave(Slave *slave);

  virtual Allocator* createAllocator();

  FrameworkID newFrameworkId();
};


// Pretty-printing of SlotOffers, Tasks, Frameworks, Slaves, etc

inline std::ostream& operator << (std::ostream& stream, const SlotOffer *o)
{
  stream << "offer " << o->id;
  return stream;
}


inline std::ostream& operator << (std::ostream& stream, const Slave *s)
{
  stream << "slave " << s->id;
  return stream;
}


inline std::ostream& operator << (std::ostream& stream, const Framework *f)
{
  stream << "framework " << f->id;
  return stream;
}


inline std::ostream& operator << (std::ostream& stream, const Task *t)
{
  stream << "task " << t->frameworkId << ":" << t->id;
  return stream;
}

}}} /* namespace */

#endif /* __MASTER_HPP__ */
