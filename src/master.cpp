#include "config.hpp" // Need to define first to get USING_ZOOKEEPER

#include <glog/logging.h>

#ifdef USING_ZOOKEEPER
#include <zookeeper.hpp>
#endif

#include "allocator.hpp"
#include "allocator_factory.hpp"
#include "master.hpp"
#include "master_webui.hpp"

using std::endl;
using std::max;
using std::min;
using std::pair;
using std::make_pair;
using std::ostringstream;
using std::map;
using std::set;
using std::string;
using std::vector;

using boost::lexical_cast;
using boost::unordered_map;
using boost::unordered_set;

using namespace nexus;
using namespace nexus::internal;
using namespace nexus::internal::master;


/* List of ZooKeeper host:port pairs (from master_main.cpp/local.cpp). */
extern string zookeeper;

namespace {

#ifdef USING_ZOOKEEPER
class MasterWatcher : public Watcher
{
private:
  Master *master;

public:
  void process(ZooKeeper *zk, int type, int state, const string &path)
  {
    // Connected!
    if ((state == ZOO_CONNECTED_STATE) &&
	(type == ZOO_SESSION_EVENT)) {
      // Create znode for master identification.
      string znode = "/home/nexus/master";
      string dirname = "/home/nexus";
      string delimiter = "/";
      string contents = "";

      int ret;
      string result;

      // Create directory path znodes as necessary.
      size_t index = dirname.find(delimiter, 0);
      while (index < string::npos) {
	index = dirname.find(delimiter, index+1);
	string prefix = dirname.substr(0, index);
	ret = zk->create(prefix, contents, ZOO_CREATOR_ALL_ACL,
			 0, &result);
	if (ret != ZOK && ret != ZNODEEXISTS)
	  fatal("failed to create ZooKeeper znode! (%s)", zk->error(ret));
      }

      // Now create znode.
      ret = zk->create(znode, master->getPID(), ZOO_CREATOR_ALL_ACL,
		       ZOO_EPHEMERAL, &result);

      // If node already exists, update its value.
      if (ret == ZNODEEXISTS)
	ret = zk->set(znode, master->getPID(), -1);

      if (ret != ZOK)
	fatal("failed to create ZooKeeper znode! (%s)", zk->error(ret));
    } else if ((state == ZOO_EXPIRED_SESSION_STATE) &&
	       (type == ZOO_SESSION_EVENT)) {
      // TODO(benh): Reconnect if session expires. Note the Zookeeper
      // C library retries in the case of connection timeouts,
      // connection loss, etc. Only expired sessions require
      // explicitly reconnecting.
	fatal("connection to ZooKeeper expired!");
    } else if ((state == ZOO_CONNECTING_STATE) &&
	       (type == ZOO_SESSION_EVENT)) {
      // The client library automatically reconnects, taking into
      // account failed servers in the connection string,
      // appropriately handling the "herd effect", etc.
      LOG(INFO) << "Lost Zookeeper connection. Retrying (automagically).";
    } else {
      fatal("unhandled ZooKeeper event!");
    }
  }

  MasterWatcher(Master *_master) : master(_master) {}
};
#endif

// A process that periodically pings the master to check filter expiries, etc
class AllocatorTimer : public Tuple<Process>
{
private:
  PID master;

protected:
  void operator () ()
  {
    link(master);
    do {
      // TODO: Make timer interval configurable, and hopefully less than 1 sec
      switch (receive(1)) {
      case PROCESS_TIMEOUT:
	send(master, pack<M2M_TIMER_TICK>());
	break;
      case PROCESS_EXIT:
	return;
      }
    } while (true);
  }

public:
  AllocatorTimer(const PID &_master) : master(_master) {}
};


// A process that periodically prints frameworks' shares to a file
class SharesPrinter : public Tuple<Process>
{
protected:
  PID master;

  void operator () ()
  {
    int tick = 0;

    std::ofstream file ("/mnt/shares");
    if (!file.is_open())
      LOG(FATAL) << "Could not open /mnt/shares";

    while (true) {
      pause(1);
      send(master, pack<M2M_GET_STATE>());
      receive();
      state::MasterState *state;
      unpack<M2M_GET_STATE_REPLY>(*((int64_t *) &state));

      uint32_t total_cpus = 0;
      uint64_t total_mem = 0;

      foreach (state::Slave *s, state->slaves) {
        total_cpus += s->cpus;
        total_mem += s->mem;
      }
      
      if (state->frameworks.empty()) {
        file << "--------------------------------" << std::endl;
      } else {
        foreach (state::Framework *f, state->frameworks) {
          double cpu_share = f->cpus / (double) total_cpus;
          double mem_share = f->mem / (double) total_mem;
          double max_share = max(cpu_share, mem_share);
          file << tick << "#" << f->id << "#" << f->name << "#" 
               << f->cpus << "#" << f->mem << "#"
               << cpu_share << "#" << mem_share << "#" << max_share << endl;
        }
      }
      delete state;
      tick++;
    }
    file.close();
  }

public:
  SharesPrinter(const PID &_master) : master(_master) {}
  ~SharesPrinter() {}
};

}


Master::Master()
  : nextFrameworkId(0), nextSlaveId(0), nextSlotOfferId(0),
    allocatorType("simple")
{}


Master::Master(const string& _allocatorType)
  : nextFrameworkId(0), nextSlaveId(0), nextSlotOfferId(0),
    allocatorType(_allocatorType)
{}
                   

Master::~Master()
{
  LOG(INFO) << "Shutting down master";
  delete allocator;
  foreachpair (_, Framework *framework, frameworks) {
    foreachpair(_, Task *task, framework->tasks)
      delete task;
    delete framework;
  }
  foreachpair (_, Slave *slave, slaves) {
    delete slave;
  }
  foreachpair (_, SlotOffer *offer, slotOffers) {
    delete offer;
  }
}


state::MasterState * Master::getState()
{
  std::ostringstream oss;
  oss << self();
  state::MasterState *state =
    new state::MasterState(BUILD_DATE, BUILD_USER, oss.str());

  foreachpair (_, Slave *s, slaves) {
    state::Slave *slave = new state::Slave(s->id, s->hostname, s->publicDns,
          s->resources.cpus, s->resources.mem, s->connectTime);
    state->slaves.push_back(slave);
  }

  foreachpair (_, Framework *f, frameworks) {
    state::Framework *framework = new state::Framework(f->id, f->name, 
       f->executorInfo.uri, f->resources.cpus, f->resources.mem,
       f->connectTime);
    state->frameworks.push_back(framework);
    foreachpair (_, Task *t, f->tasks) {
      state::Task *task = new state::Task(t->id, t->name, t->frameworkId,
          t->slaveId, t->state, t->resources.cpus, t->resources.mem);
      framework->tasks.push_back(task);
    }
    foreach (SlotOffer *o, f->slotOffers) {
      state::SlotOffer *offer = new state::SlotOffer(o->id, o->frameworkId);
      foreach (SlaveResources &r, o->resources) {
        state::SlaveResources *resources = new state::SlaveResources(
            r.slave->id, r.resources.cpus, r.resources.mem);
        offer->resources.push_back(resources);
      }
      framework->offers.push_back(offer);
    }
  }
  
  return state;
}


// Return connected frameworks that are not in the process of being removed
vector<Framework *> Master::getActiveFrameworks()
{
  vector <Framework *> result;
  foreachpair(_, Framework *framework, frameworks)
    if (framework->active)
      result.push_back(framework);
  return result;
}


// Return connected slaves that are not in the process of being removed
vector<Slave *> Master::getActiveSlaves()
{
  vector <Slave *> result;
  foreachpair(_, Slave *slave, slaves)
    if (slave->active)
      result.push_back(slave);
  return result;
}


Framework * Master::lookupFramework(FrameworkID fid)
{
  unordered_map<FrameworkID, Framework *>::iterator it =
    frameworks.find(fid);
  if (it != frameworks.end())
    return it->second;
  else
    return NULL;
}


Slave * Master::lookupSlave(SlaveID sid)
{
  unordered_map<SlaveID, Slave *>::iterator it =
    slaves.find(sid);
  if (it != slaves.end())
    return it->second;
  else
    return NULL;
}


SlotOffer * Master::lookupSlotOffer(OfferID oid)
{
  unordered_map<OfferID, SlotOffer *>::iterator it =
    slotOffers.find(oid);
  if (it != slotOffers.end()) 
    return it->second;
  else
    return NULL;
}


void Master::operator () ()
{
  LOG(INFO) << "Master started at " << self();

  allocator = createAllocator();
  if (!allocator)
    LOG(FATAL) << "Unrecognized allocator type: " << allocatorType;

  link(spawn(new AllocatorTimer(self())));
  //link(spawn(new SharesPrinter(self())));

#ifdef USING_ZOOKEEPER
  ZooKeeper *zk;
  if (!zookeeper.empty())
    zk = new ZooKeeper(zookeeper, 10000, new MasterWatcher(this));
#endif

  while (true) {
    switch (receive()) {

    case F2M_REGISTER_FRAMEWORK: {
      FrameworkID fid = nextFrameworkId++;
      Framework *framework = new Framework(from(), fid);
      unpack<F2M_REGISTER_FRAMEWORK>(framework->name,
                                     framework->user,
                                     framework->executorInfo);
      LOG(INFO) << "Registering " << framework << " at " << framework->pid;
      frameworks[fid] = framework;
      pidToFid[framework->pid] = fid;
      link(framework->pid);
      send(framework->pid, pack<M2F_REGISTER_REPLY>(fid));
      allocator->frameworkAdded(framework);
      if (framework->executorInfo.uri == "")
        terminateFramework(framework, 1, "No executor URI given");
      break;
    }

    case F2M_UNREGISTER_FRAMEWORK: {
      FrameworkID fid;
      unpack<F2M_UNREGISTER_FRAMEWORK>(fid);
      LOG(INFO) << "Asked to unregister framework " << fid;
      Framework *framework = lookupFramework(fid);
      if (framework != NULL)
        removeFramework(framework);
      break;
    }

    case F2M_SLOT_OFFER_REPLY: {
      FrameworkID fid;
      OfferID oid;
      vector<TaskDescription> tasks;
      Params params;
      unpack<F2M_SLOT_OFFER_REPLY>(fid, oid, tasks, params);
      Framework *framework = lookupFramework(fid);
      if (framework != NULL) {
        SlotOffer *offer = lookupSlotOffer(oid);
        if (offer != NULL) {
          processOfferReply(offer, tasks, params);
        } else {
          // The slot offer is gone, meaning that we rescinded it or that
          // the slave was lost; immediately report any tasks in it as lost
          foreach (TaskDescription &t, tasks) {
            send(framework->pid,
                 pack<M2F_STATUS_UPDATE>(t.taskId, TASK_LOST, ""));
          }
        }
      }
      break;
    }

    case F2M_REVIVE_OFFERS: {
      FrameworkID fid;
      unpack<F2M_REVIVE_OFFERS>(fid);
      Framework *framework = lookupFramework(fid);
      if (framework != NULL) {
        LOG(INFO) << "Reviving offers for " << framework;
        framework->slaveFilter.clear();
        allocator->offersRevived(framework);
      }
      break;
    }

    case F2M_KILL_TASK: {
      FrameworkID fid;
      TaskID tid;
      unpack<F2M_KILL_TASK>(fid, tid);
      Framework *framework = lookupFramework(fid);
      if (framework != NULL) {
        Task *task = framework->lookupTask(tid);
        if (task != NULL) {
          LOG(INFO) << "Asked to kill " << task << " by its framework";
          killTask(task);
        }
      }
      break;
    }

    case F2M_FRAMEWORK_MESSAGE: {
      FrameworkID fid;
      FrameworkMessage message;
      unpack<F2M_FRAMEWORK_MESSAGE>(fid, message);
      Framework *framework = lookupFramework(fid);
      if (framework != NULL) {
        Slave *slave = lookupSlave(message.slaveId);
        if (slave != NULL) {
          LOG(INFO) << "Sending framework message to " << slave;
          send(slave->pid, pack<M2S_FRAMEWORK_MESSAGE>(fid, message));
        }
      }
      break;
    }

    case S2M_REGISTER_SLAVE: {
      Slave *slave = new Slave(from(), nextSlaveId++);
      unpack<S2M_REGISTER_SLAVE>(slave->hostname, slave->publicDns,
          slave->resources);
      LOG(INFO) << "Registering " << slave << " at " << slave->pid;
      slaves[slave->id] = slave;
      pidToSid[slave->pid] = slave->id;
      link(slave->pid);
      send(slave->pid, pack<M2S_REGISTER_REPLY>(slave->id));
      allocator->slaveAdded(slave);
      break;
    }

    case S2M_UNREGISTER_SLAVE: {
      SlaveID sid;
      unpack<S2M_UNREGISTER_SLAVE>(sid);
      LOG(INFO) << "Asked to unregister slave " << sid;
      Slave *slave = lookupSlave(sid);
      if (slave != NULL)
        removeSlave(slave);
      break;
    }

    case S2M_STATUS_UPDATE: {
      SlaveID sid;
      FrameworkID fid;
      TaskID tid;
      TaskState state;
      string data;
      unpack<S2M_STATUS_UPDATE>(sid, fid, tid, state, data);
      if (Slave *slave = lookupSlave(sid)) {
        if (Framework *framework = lookupFramework(fid)) {
          // Pass on the status update to the framework
          send(framework->pid, pack<M2F_STATUS_UPDATE>(tid, state, data));
          // Update the task state locally
          Task *task = slave->lookupTask(fid, tid);
          if (task != NULL) {
            LOG(INFO) << "Status update: " << task << " is in state " << state;
            task->state = state;
            // Remove the task if it finished or failed
            if (state == TASK_FINISHED || state == TASK_FAILED ||
                state == TASK_KILLED || state == TASK_LOST) {
              LOG(INFO) << "Removing " << task << " because it's done";
              removeTask(task, TRR_TASK_ENDED);
            }
          }
        }
        break;
      }
    }
      
    case S2M_FRAMEWORK_MESSAGE: {
      SlaveID sid;
      FrameworkID fid;
      FrameworkMessage message;
      unpack<S2M_FRAMEWORK_MESSAGE>(sid, fid, message);
      Slave *slave = lookupSlave(sid);
      if (slave != NULL) {
        Framework *framework = lookupFramework(fid);
        if (framework != NULL)
          send(framework->pid, pack<M2F_FRAMEWORK_MESSAGE>(message));
      }
      break;
    }

    case S2M_LOST_EXECUTOR: {
      SlaveID sid;
      FrameworkID fid;
      int32_t status;
      unpack<S2M_LOST_EXECUTOR>(sid, fid, status);
      Slave *slave = lookupSlave(sid);
      if (slave != NULL) {
        Framework *framework = lookupFramework(fid);
        if (framework != NULL) {
          ostringstream oss;
          if (status == -1) {
            oss << "Executor on " << slave << " (" << slave->hostname
                << ") disconnected";
          } else {
            oss << "Executor on " << slave << " (" << slave->hostname
                << ") exited with status " << status;
          }
          terminateFramework(framework, status, oss.str());
        }
      }
      break;
    }

    case SH2M_HEARTBEAT: {
      SlaveID sid;
      unpack<SH2M_HEARTBEAT>(sid);
      Slave *slave = lookupSlave(sid);
      if (slave != NULL)
        //LOG(INFO) << "Received heartbeat for " << slave << " from " << from();
        ;
      else
        LOG(WARNING) << "Received heartbeat for UNKNOWN slave " << sid
                     << " from " << from();
      break;
    }

    case M2M_TIMER_TICK: {
      LOG(INFO) << "Allocator timer tick";
      foreachpair (_, Framework *framework, frameworks)
        framework->removeExpiredFilters();
      allocator->timerTick();
      break;
    }

    case PROCESS_EXIT: {
      LOG(INFO) << "Process exited: " << from();
      if (pidToFid.find(from()) != pidToFid.end()) {
        FrameworkID fid = pidToFid[from()];
        if (Framework *framework = lookupFramework(fid)) {
          LOG(INFO) << framework << " disconnected";
          removeFramework(framework);
        }
      } else if (pidToSid.find(from()) != pidToSid.end()) {
        SlaveID sid = pidToSid[from()];
        if (Slave *slave = lookupSlave(sid)) {
          LOG(INFO) << slave << " disconnected";
          removeSlave(slave);
        }
      }
      break;
    }

    case M2M_GET_STATE: {
      send(from(), pack<M2M_GET_STATE_REPLY>((int64_t) getState()));
      break;
    }
    
    case M2M_SHUTDOWN: {
      LOG(INFO) << "Asked to shut down by " << from();
      foreachpair (_, Slave *slave, slaves)
        send(slave->pid, pack<M2S_SHUTDOWN>());
      return;
    }

    default:
      LOG(ERROR) << "Received unknown MSGID " << msgid() << " from " << from();
      break;
    }
  }
}


OfferID Master::makeOffer(Framework *framework,
                          const vector<SlaveResources>& resources)
{
  OfferID oid = nextSlotOfferId++;
  SlotOffer *offer = new SlotOffer(oid, framework->id, resources);
  slotOffers[offer->id] = offer;
  framework->addOffer(offer);
  foreach (const SlaveResources& r, resources) {
    r.slave->slotOffers.insert(offer);
    r.slave->resourcesOffered += r.resources;
  }
  LOG(INFO) << "Sending " << offer << " to " << framework;
  vector<SlaveOffer> offers;
  foreach (const SlaveResources& r, resources) {
    Params params;
    params.set("cpus", r.resources.cpus);
    params.set("mem", r.resources.mem);
    SlaveOffer offer(r.slave->id, r.slave->hostname, params.getMap());
    offers.push_back(offer);
  }
  send(framework->pid, pack<M2F_SLOT_OFFER>(oid, offers));
  return oid;
}


// Process a resource offer reply (for a non-cancelled offer) by launching
// the desired tasks (if the offer contains a valid set of tasks) and
// reporting any unused resources to the allocator
void Master::processOfferReply(SlotOffer *offer,
    const vector<TaskDescription>& tasks, const Params& params)
{
  Framework *framework = lookupFramework(offer->frameworkId);
  CHECK(framework != NULL);

  // Count resources in the offer
  unordered_map<Slave *, Resources> offerResources;
  foreach (SlaveResources &r, offer->resources) {
    offerResources[r.slave] = r.resources;
  }

  // Count resources in the response, and check that its tasks are valid
  unordered_map<Slave *, Resources> responseResources;
  foreach (const TaskDescription &t, tasks) {
    // Check whether this task size is valid
    Params params(t.params);
    Resources res(params.getInt32("cpus", -1),
                  params.getInt64("mem", -1));
    if (res.cpus < MIN_CPUS || res.mem < MIN_MEM || 
        res.cpus > MAX_CPUS || res.mem > MAX_MEM) {
      terminateFramework(framework, 0,
          "Invalid task size: " + lexical_cast<string>(res));
      return;
    }
    // Check whether the task is on a valid slave
    Slave *slave = lookupSlave(t.slaveId);
    if (!slave || offerResources.find(slave) == offerResources.end()) {
      terminateFramework(framework, 0, "Invalid slave in offer reply");
      return;
    }
    responseResources[slave] += res;
  }

  // Check that the total accepted on each slave isn't more than offered
  foreachpair (Slave *s, Resources& respRes, responseResources) {
    Resources &offRes = offerResources[s];
    if (respRes.cpus > offRes.cpus || respRes.mem > offRes.mem) {
      terminateFramework(framework, 0, "Too many resources accepted");
      return;
    }
  }

  // Check that there are no duplicate task IDs
  unordered_set<TaskID> idsInResponse;
  foreach (const TaskDescription &t, tasks) {
    if (framework->tasks.find(t.taskId) != framework->tasks.end() ||
        idsInResponse.find(t.taskId) != idsInResponse.end()) {
      terminateFramework(framework, 0,
          "Duplicate task ID: " + lexical_cast<string>(t.taskId));
      return;    
    }
    idsInResponse.insert(t.taskId);
  }

  // Launch the tasks in the response
  foreach (const TaskDescription &t, tasks) {
    launchTask(framework, t);
  }

  // If there are resources left on some slaves, add filters for them
  vector<SlaveResources> resourcesLeft;
  int timeout = params.getInt32("timeout", DEFAULT_REFUSAL_TIMEOUT);
  time_t expiry = (timeout == -1) ? 0 : time(0) + timeout;
  foreachpair (Slave *s, Resources offRes, offerResources) {
    Resources respRes = responseResources[s];
    Resources left = offRes - respRes;
    if (left.cpus > 0 || left.mem > 0) {
      resourcesLeft.push_back(SlaveResources(s, left));
    }
    if (timeout != 0 && respRes.cpus == 0 && respRes.mem == 0) {
      LOG(INFO) << "Adding filter on " << s << " to " << framework
                << " for  " << timeout << " seconds";
      framework->slaveFilter[s] = expiry;
    }
  }
  
  // Return the resources left to the allocator
  removeSlotOffer(offer, ORR_FRAMEWORK_REPLIED, resourcesLeft);
}


void Master::launchTask(Framework *f, const TaskDescription& t)
{
  Params params(t.params);
  Resources res(params.getInt32("cpus", -1),
                params.getInt64("mem", -1));
  Slave *slave = lookupSlave(t.slaveId);
  Task *task = f->addTask(t.taskId, t.name, slave->id, res);
  LOG(INFO) << "Launching " << task << " on " << slave;
  slave->addTask(task);
  allocator->taskAdded(task);
  send(slave->pid, pack<M2S_RUN_TASK>(
        f->id, t.taskId, f->name, f->user, f->executorInfo,
        t.name, t.arg, t.params));
}


void Master::rescindOffer(SlotOffer *offer)
{
  removeSlotOffer(offer, ORR_OFFER_RESCINDED, offer->resources);
}


void Master::killTask(Task *task)
{
  LOG(INFO) << "Killing " << task;
  Framework *framework = lookupFramework(task->frameworkId);
  Slave *slave = lookupSlave(task->slaveId);
  CHECK(framework != NULL);
  CHECK(slave != NULL);
  send(slave->pid, pack<M2S_KILL_TASK>(framework->id, task->id));
  send(framework->pid,
       pack<M2F_STATUS_UPDATE>(task->id, TASK_KILLED, task->message));
  removeTask(task, TRR_TASK_ENDED);
}


// Terminate a framework, sending it a particular error message
// TODO: Make the error codes and messages programmer-friendly
void Master::terminateFramework(Framework *framework,
                                int32_t code,
                                const std::string& message)
{
  LOG(INFO) << "Terminating " << framework << " due to error: " << message;
  send(framework->pid, pack<M2F_ERROR>(code, message));
  removeFramework(framework);
}


// Remove a slot offer (because it was replied or we lost a framework or slave)
void Master::removeSlotOffer(SlotOffer *offer,
                             OfferReturnReason reason,
                             const vector<SlaveResources>& resourcesLeft)
{
  // Remove from slaves
  foreach (SlaveResources& r, offer->resources) {
    CHECK(r.slave != NULL);
    r.slave->resourcesOffered -= r.resources;
    r.slave->slotOffers.erase(offer);
  }
    
  // Remove from framework
  Framework *framework = lookupFramework(offer->frameworkId);
  CHECK(framework != NULL);
  framework->removeOffer(offer);
  // Also send framework a rescind message unless the reason we are
  // removing the offer is that the framework replied to it
  if (reason != ORR_FRAMEWORK_REPLIED) {
    send(framework->pid, pack<M2F_RESCIND_OFFER>(offer->id));
  }
  
  // Tell the allocator about the resources freed up
  allocator->offerReturned(offer, reason, resourcesLeft);
  
  // Delete it
  slotOffers.erase(offer->id);
  delete offer;
}


// Kill all of a framework's tasks, delete the framework object, and
// reschedule slot offers for slots that were assigned to this framework
void Master::removeFramework(Framework *framework)
{ 
  framework->active = false;
  // TODO: Notify allocator that a framework removal is beginning?
  
  // Tell slaves to kill the framework
  foreachpair (_, Slave *slave, slaves)
    send(slave->pid, pack<M2S_KILL_FRAMEWORK>(framework->id));

  // Remove pointers to the framework's tasks in slaves
  unordered_map<TaskID, Task *> tasksCopy = framework->tasks;
  foreachpair (_, Task *task, tasksCopy) {
    Slave *slave = lookupSlave(task->slaveId);
    CHECK(slave != NULL);
    removeTask(task, TRR_FRAMEWORK_LOST);
  }
  
  // Remove the framework's slot offers
  unordered_set<SlotOffer *> slotOffersCopy = framework->slotOffers;
  foreach (SlotOffer* offer, slotOffersCopy) {
    removeSlotOffer(offer, ORR_FRAMEWORK_LOST, offer->resources);
  }

  // TODO(benh): unlink(framework->pid);
  pidToFid.erase(framework->pid);

  // Delete it
  frameworks.erase(framework->id);
  allocator->frameworkRemoved(framework);
  delete framework;
}


// Lose all of a slave's tasks and delete the slave object
void Master::removeSlave(Slave *slave)
{ 
  slave->active = false;
  // TODO: Notify allocator that a slave removal is beginning?
  
  // Remove pointers to slave's tasks in frameworks, and send status updates
  unordered_map<pair<FrameworkID, TaskID>, Task *> tasksCopy = slave->tasks;
  foreachpair (_, Task *task, tasksCopy) {
    Framework *framework = lookupFramework(task->frameworkId);
    CHECK(framework != NULL);
    send(framework->pid, pack<M2F_STATUS_UPDATE>(task->id, TASK_LOST,
                                                 task->message));
    removeTask(task, TRR_SLAVE_LOST);
  }

  // Remove slot offers from the slave; this will also rescind them
  unordered_set<SlotOffer *> slotOffersCopy = slave->slotOffers;
  foreach (SlotOffer *offer, slotOffersCopy) {
    // Only report resources on slaves other than this one to the allocator
    vector<SlaveResources> otherSlaveResources;
    foreach (SlaveResources& r, offer->resources) {
      if (r.slave != slave) {
        otherSlaveResources.push_back(r);
      }
    }
    removeSlotOffer(offer, ORR_SLAVE_LOST, otherSlaveResources);
  }
  
  // Remove slave from any filters
  foreachpair (_, Framework *framework, frameworks)
    framework->slaveFilter.erase(slave);
  
  // Send lost-slave message to all frameworks (this helps them re-run
  // previously finished tasks whose output was on the lost slave)
  foreachpair (_, Framework *framework, frameworks)
    send(framework->pid, pack<M2F_LOST_SLAVE>(slave->id));

  // TODO(benh): unlink(slave->pid);
  pidToSid.erase(slave->pid);

  // Delete it
  slaves.erase(slave->id);
  allocator->slaveRemoved(slave);
  delete slave;
}


// Remove a slot offer (because it was replied or we lost a framework or slave)
void Master::removeTask(Task *task, TaskRemovalReason reason)
{
  Framework *framework = lookupFramework(task->frameworkId);
  Slave *slave = lookupSlave(task->slaveId);
  CHECK(framework != NULL);
  CHECK(slave != NULL);
  framework->removeTask(task->id);
  slave->removeTask(task);
  allocator->taskRemoved(task, reason);
  delete task;
}


Allocator* Master::createAllocator()
{
  LOG(INFO) << "Creating \"" << allocatorType << "\" allocator";
  return AllocatorFactory::instantiate(allocatorType, this);
}
