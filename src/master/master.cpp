#include <iomanip>

#include <glog/logging.h>

#include <google/protobuf/descriptor.h>

#include "common/date_utils.hpp"

#include "allocator.hpp"
#include "allocator_factory.hpp"
#include "master.hpp"
#include "webui.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;

using boost::lexical_cast;
using boost::unordered_map;
using boost::unordered_set;

using std::endl;
using std::max;
using std::ostringstream;
using std::pair;
using std::set;
using std::setfill;
using std::setw;
using std::string;
using std::vector;


namespace {

// A process that periodically pings the master to check filter expiries, etc
class AllocatorTimer : public MesosProcess
{
public:
  AllocatorTimer(const PID &_master) : master(_master) {}

protected:
  virtual void operator () ()
  {
    link(master);
    do {
      switch (receive(1)) {
      case PROCESS_TIMEOUT:
	send(master, M2M_TIMER_TICK);
	break;
      case PROCESS_EXIT:
	return;
      }
    } while (true);
  }

private:
  const PID master;
};


// A process that periodically prints frameworks' shares to a file
class SharesPrinter : public MesosProcess
{
public:
  SharesPrinter(const PID &_master) : master(_master) {}
  ~SharesPrinter() {}

protected:
  virtual void operator () ()
  {
    int tick = 0;

    std::ofstream file ("/mnt/shares");
    if (!file.is_open())
      LOG(FATAL) << "Could not open /mnt/shares";

    while (true) {
      pause(1);

      send(master, M2M_GET_STATE);
      receive();
      CHECK(msgid() == M2M_GET_STATE_REPLY);

      const Message<M2M_GET_STATE_REPLY>& msg = message();

      state::MasterState *state =
        *(state::MasterState **) msg.pointer().data();

      uint32_t total_cpus = 0;
      uint32_t total_mem = 0;

      foreach (state::Slave *s, state->slaves) {
        total_cpus += s->cpus;
        total_mem += s->mem;
      }
      
      if (state->frameworks.empty()) {
        file << "--------------------------------" << endl;
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

private:
  const PID master;
};

}


Master::Master()
  : nextFrameworkId(0), nextSlaveId(0), nextOfferId(0)
{
  allocatorType = "simple";
}


Master::Master(const Configuration& _conf)
  : conf(_conf), nextFrameworkId(0), nextSlaveId(0), nextOfferId(0)
{
  allocatorType = conf.get("allocator", "simple");
}
                   

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


void Master::registerOptions(Configurator* configurator)
{
  configurator->addOption<string>("allocator", 'a', "Allocation module name",
                                  "simple");
  configurator->addOption<bool>("root_submissions",
                                "Can root submit frameworks?",
                                true);
}


state::MasterState * Master::getState()
{
  state::MasterState *state =
    new state::MasterState(BUILD_DATE, BUILD_USER, self());

  foreachpair (_, Slave *s, slaves) {
    Resources resources(s->info.resources());
    Resource::Scalar cpus;
    Resource::Scalar mem;
    cpus.set_value(-1);
    mem.set_value(-1);
    cpus = resources.getScalar("cpus", cpus);
    mem = resources.getScalar("mem", mem);

    state::Slave *slave =
      new state::Slave(s->slaveId.value(), s->info.hostname(),
                       s->info.public_hostname(), cpus.value(),
                       mem.value(), s->connectTime);

    state->slaves.push_back(slave);
  }

  foreachpair (_, Framework *f, frameworks) {
    Resources resources(f->resources);
    Resource::Scalar cpus;
    Resource::Scalar mem;
    cpus.set_value(-1);
    mem.set_value(-1);
    cpus = resources.getScalar("cpus", cpus);
    mem = resources.getScalar("mem", mem);

    state::Framework *framework =
      new state::Framework(f->frameworkId.value(), f->info.user(),
                           f->info.name(), f->info.executor().uri(),
                           cpus.value(), mem.value(), f->connectTime);

    state->frameworks.push_back(framework);

    foreachpair (_, Task *t, f->tasks) {
      Resources resources(t->resources());
      Resource::Scalar cpus;
      Resource::Scalar mem;
      cpus.set_value(-1);
      mem.set_value(-1);
      cpus = resources.getScalar("cpus", cpus);
      mem = resources.getScalar("mem", mem);

      state::Task *task =
        new state::Task(t->task_id().value(), t->name(),
                        t->framework_id().value(), t->slave_id().value(),
                        TaskState_descriptor()->FindValueByNumber(t->state())->name(),
                        cpus.value(), mem.value());

      framework->tasks.push_back(task);
    }

    foreach (SlotOffer *o, f->slotOffers) {
      state::SlotOffer *offer =
        new state::SlotOffer(o->offerId.value(), o->frameworkId.value());

      foreach (const SlaveResources &r, o->resources) {
        Resources resources(r.resources);
        Resource::Scalar cpus;
        Resource::Scalar mem;
        cpus.set_value(-1);
        mem.set_value(-1);
        cpus = resources.getScalar("cpus", cpus);
        mem = resources.getScalar("mem", mem);

        state::SlaveResources *sr =
          new state::SlaveResources(r.slave->slaveId.value(),
                                    cpus.value(), mem.value());

        offer->resources.push_back(sr);
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


Framework * Master::lookupFramework(const FrameworkID& frameworkId)
{
  unordered_map<FrameworkID, Framework *>::iterator it =
    frameworks.find(frameworkId);
  if (it != frameworks.end())
    return it->second;
  else
    return NULL;
}


Slave * Master::lookupSlave(const SlaveID& slaveId)
{
  unordered_map<SlaveID, Slave *>::iterator it =
    slaves.find(slaveId);
  if (it != slaves.end())
    return it->second;
  else
    return NULL;
}


SlotOffer * Master::lookupSlotOffer(const OfferID& offerId)
{
  unordered_map<OfferID, SlotOffer *>::iterator it =
    slotOffers.find(offerId);
  if (it != slotOffers.end()) 
    return it->second;
  else
    return NULL;
}


void Master::operator () ()
{
  LOG(INFO) << "Master started at mesos://" << self();

  active = false;

  // Don't do anything until we get a master token.
  while (receive() != GOT_MASTER_TOKEN) {
    LOG(INFO) << "Oops! We're dropping a message since "
              << "we haven't received an identifier yet!";  
  }

  const Message<GOT_MASTER_TOKEN>& msg = message();

  // The master ID is comprised of the current date and some ephemeral
  // token (e.g., determined by ZooKeeper).

  masterId = DateUtils::currentDate() + "-" + msg.token();
  LOG(INFO) << "Master ID: " << masterId;

  // Create the allocator (we do this after the constructor because it
  // leaks 'this').
  allocator = createAllocator();
  if (!allocator)
    LOG(FATAL) << "Unrecognized allocator type: " << allocatorType;

  link(spawn(new AllocatorTimer(self())));
  //link(spawn(new SharesPrinter(self())));

  while (true) {
    switch (receive()) {

    case NEW_MASTER_DETECTED: {
      const Message<NEW_MASTER_DETECTED>& msg = message();

      // Check and see if we are (1) still waiting to be the active
      // master, (2) newly active master, (3) no longer active master,
      // or (4) still active master.
      PID pid(msg.pid());

      if (pid != self() && !active) {
	LOG(INFO) << "Waiting to be master!";
      } else if (pid == self() && !active) {
	LOG(INFO) << "Acting as master!";
	active = true;
      } else if (pid != self() && active) {
	LOG(FATAL) << "No longer active master ... committing suicide!";
      } else if (pid == self() && active) {
	LOG(INFO) << "Still acting as master!";
      }
      break;
    }

    case NO_MASTER_DETECTED: {
      if (active) {
	LOG(FATAL) << "No longer active master ... committing suicide!";
      } else {
	LOG(FATAL) << "No master detected (?) ... committing suicide!";
      }
      break;
    }

    case MASTER_DETECTION_FAILURE: {
      LOG(FATAL) << "Cannot reliably detect master ... committing suicide!";
      break;
    }

    case F2M_REGISTER_FRAMEWORK: {
      const Message<F2M_REGISTER_FRAMEWORK>& msg = message();

      Framework *framework =
        new Framework(msg.framework(), newFrameworkId(), from(), elapsed());

      LOG(INFO) << "Registering " << framework << " at " << framework->pid;

      if (framework->info.executor().uri() == "") {
        LOG(INFO) << framework << " registering without an executor URI";
        Message<M2F_ERROR> out;
        out.set_code(1);
        out.set_message("No executor URI given");
        send(from(), out);
        delete framework;
        break;
      }

      bool rootSubmissions = conf.get<bool>("root_submissions", true);
      if (framework->info.user() == "root" && rootSubmissions == false) {
        LOG(INFO) << framework << " registering as root, but "
                  << "root submissions are disabled on this cluster";
        Message<M2F_ERROR> out;
        out.set_code(1);
        out.set_message("User 'root' is not allowed to run frameworks");
        send(from(), out);
        delete framework;
        break;
      }

      addFramework(framework);
      break;
    }

    case F2M_REREGISTER_FRAMEWORK: {
      const Message<F2M_REREGISTER_FRAMEWORK> &msg = message();

      if (msg.framework_id() == "") {
        LOG(ERROR) << "Framework re-registering without an id!";
        Message<M2F_ERROR> out;
        out.set_code(1);
        out.set_message("Missing framework id");
        send(from(), out);
        break;
      }

      if (msg.framework().executor().uri() == "") {
        LOG(INFO) << "Framework " << msg.framework_id() << " re-registering "
                  << "without an executor URI";
        Message<M2F_ERROR> out;
        out.set_code(1);
        out.set_message("No executor URI given");
        send(from(), out);
        break;
      }

      LOG(INFO) << "Re-registering framework " << msg.framework_id()
                << " at " << from();

      if (frameworks.count(msg.framework_id()) > 0) {
        // Using the "generation" of the scheduler allows us to keep a
        // scheduler that got partitioned but didn't die (in ZooKeeper
        // speak this means didn't lose their session) and then
        // eventually tried to connect to this master even though
        // another instance of their scheduler has reconnected. This
        // might not be an issue in the future when the
        // master/allocator launches the scheduler can get restarted
        // (if necessary) by the master and the master will always
        // know which scheduler is the correct one.
        if (msg.generation() == 0) {
          LOG(INFO) << "Framework " << msg.framework_id() << " failed over";
          failoverFramework(frameworks[msg.framework_id()], from());
          // TODO: Should we check whether the new scheduler has given
          // us a different framework name, user name or executor info?
        } else {
          LOG(INFO) << "Framework " << msg.framework_id()
                    << " re-registering with an already used id "
                    << " and not failing over!";
          Message<M2F_ERROR> out;
          out.set_code(1);
          out.set_message("Framework id in use");
          send(from(), out);
          break;
        }
      } else {
        // We don't have a framework with this ID, so we must be a newly
        // elected Mesos master to which either an existing scheduler or a
        // failed-over one is connecting. Create a Framework object and add
        // any tasks it has that have been reported by reconnecting slaves.
        Framework *framework =
          new Framework(msg.framework(), msg.framework_id(), from(), elapsed());

        // TODO(benh): Check for root submissions like above!

        addFramework(framework);
        // Add any running tasks reported by slaves for this framework.
        foreachpair (const SlaveID& slaveId, Slave *slave, slaves) {
          foreachpair (_, Task *task, slave->tasks) {
            if (framework->frameworkId == task->framework_id()) {
              framework->addTask(task);
            }
          }
        }
      }

      CHECK(frameworks.count(msg.framework_id()) > 0);

      // Broadcast the new framework pid to all the slaves. We have to
      // broadcast because an executor might be running on a slave but
      // it currently isn't running any tasks. This could be a
      // potential scalability issue ...
      foreachpair (_, Slave *slave, slaves) {
        Message<M2S_UPDATE_FRAMEWORK> out;
        out.mutable_framework_id()->MergeFrom(msg.framework_id());
        out.set_pid(from());
        send(slave->pid, out);
      }
      break;
    }

    case F2M_UNREGISTER_FRAMEWORK: {
      const Message<F2M_UNREGISTER_FRAMEWORK>& msg = message();

      LOG(INFO) << "Asked to unregister framework " << msg.framework_id();

      Framework *framework = lookupFramework(msg.framework_id());
      if (framework != NULL) {
        if (framework->pid == from())
          removeFramework(framework);
        else
          LOG(WARNING) << from() << " tried to unregister framework; "
                       << "expecting " << framework->pid;
      }
      break;
    }

    case F2M_RESOURCE_OFFER_REPLY: {
      const Message<F2M_RESOURCE_OFFER_REPLY>& msg = message();

      Framework *framework = lookupFramework(msg.framework_id());
      if (framework != NULL) {

        // Copy out the task descriptions (could optimize).
        vector<TaskDescription> tasks;
        for (int i = 0; i < msg.task_size(); i++) {
          tasks.push_back(msg.task(i));
        }

        SlotOffer *offer = lookupSlotOffer(msg.offer_id());
        if (offer != NULL) {
          processOfferReply(offer, tasks, msg.params());
        } else {
          // The slot offer is gone, meaning that we rescinded it, it
          // has already been replied to, or that the slave was lost;
          // immediately report any tasks in it as lost (it would
          // probably be better to have better error messages here).
          foreach (const TaskDescription &task, tasks) {
            Message<M2F_STATUS_UPDATE> out;
            out.mutable_framework_id()->MergeFrom(msg.framework_id());
            TaskStatus *status = out.mutable_status();
            status->mutable_task_id()->MergeFrom(task.task_id());
            status->mutable_slave_id()->MergeFrom(task.slave_id());
            status->set_state(TASK_LOST);
            send(framework->pid, out);
          }
        }
      }
      break;
    }

    case F2M_REVIVE_OFFERS: {
      const Message<F2M_REVIVE_OFFERS>& msg = message();

      Framework *framework = lookupFramework(msg.framework_id());
      if (framework != NULL) {
        LOG(INFO) << "Reviving offers for " << framework;
        framework->slaveFilter.clear();
        allocator->offersRevived(framework);
      }
      break;
    }

    case F2M_KILL_TASK: {
      const Message<F2M_KILL_TASK>& msg = message();

      LOG(INFO) << "Asked to kill task " << msg.task_id()
		<< " of framework " << msg.framework_id();

      Framework *framework = lookupFramework(msg.framework_id());
      if (framework != NULL) {
        Task *task = framework->lookupTask(msg.task_id());
        if (task != NULL) {
          killTask(task);
	} else {
	  LOG(ERROR) << "Cannot kill task " << msg.task_id()
		     << " of framework " << msg.framework_id()
		     << " because it cannot be found";
          Message<M2F_STATUS_UPDATE> out;
          out.mutable_framework_id()->MergeFrom(task->framework_id());
          TaskStatus *status = out.mutable_status();
          status->mutable_task_id()->MergeFrom(task->task_id());
          status->mutable_slave_id()->MergeFrom(task->slave_id());
          status->set_state(TASK_LOST);
          send(framework->pid, out);
        }
      }
      break;
    }

    case F2M_FRAMEWORK_MESSAGE: {
      const Message<F2M_FRAMEWORK_MESSAGE>& msg = message();

      Framework *framework = lookupFramework(msg.framework_id());
      if (framework != NULL) {
        Slave *slave = lookupSlave(msg.message().slave_id());
        if (slave != NULL) {
          Message<M2S_FRAMEWORK_MESSAGE> out;
          out.mutable_framework_id()->MergeFrom(msg.framework_id());
          out.mutable_message()->MergeFrom(msg.message());
          send(slave->pid, out);
        }
      }
      break;
    }

    case S2M_REGISTER_SLAVE: {
      const Message<S2M_REGISTER_SLAVE>& msg = message();

      Slave* slave = new Slave(msg.slave(), newSlaveId(), from(), elapsed());

      LOG(INFO) << "Registering slave " << slave->slaveId
                << " at " << slave->pid;

      slaves[slave->slaveId] = slave;
      pidToSlaveId[slave->pid] = slave->slaveId;
      link(slave->pid);

      allocator->slaveAdded(slave);

      Message<M2S_REGISTER_REPLY> out;
      out.mutable_slave_id()->MergeFrom(slave->slaveId);
      out.set_heartbeat_interval(HEARTBEAT_INTERVAL);
      send(slave->pid, out);
      break;
    }

    case S2M_REREGISTER_SLAVE: {
      const Message<S2M_REREGISTER_SLAVE>& msg = message();

      LOG(INFO) << "Re-registering " << msg.slave_id() << " at " << from();

      if (msg.slave_id() == "") {
        LOG(ERROR) << "Slave re-registered without a SlaveID!";
        send(from(), M2S_SHUTDOWN);
        break;
      }

      // TODO(benh): Once we support handling session expiration, we
      // will want to handle having a slave re-register with us when
      // we already have them recorded (i.e., the below if statement
      // will evaluate to true).

      if (lookupSlave(msg.slave_id()) != NULL) {
        LOG(ERROR) << "Slave re-registered with in use SlaveID!";
        send(from(), M2S_SHUTDOWN);
        break;
      }

      Slave* slave = new Slave(msg.slave(), msg.slave_id(), from(), elapsed());

      slaves[slave->slaveId] = slave;
      pidToSlaveId[slave->pid] = slave->slaveId;
      link(slave->pid);

      Message<M2S_REREGISTER_REPLY> out;
      out.mutable_slave_id()->MergeFrom(slave->slaveId);
      out.set_heartbeat_interval(HEARTBEAT_INTERVAL);
      send(slave->pid, out);

      for (int i = 0; i < msg.task_size(); i++) {
        Task *task = new Task(msg.task(i));
        slave->addTask(task);

        // Tell this slave the current framework pid for this task.
        Framework *framework = lookupFramework(task->framework_id());
        if (framework != NULL) {
          framework->addTask(task);
          Message<M2S_UPDATE_FRAMEWORK> out;
          out.mutable_framework_id()->MergeFrom(framework->frameworkId);
          out.set_pid(framework->pid);
          send(slave->pid, out);
        }
      }

      // TODO(benh|alig): We should put a timeout on how long we keep
      // tasks running that never have frameworks reregister that
      // claim them.
      break;
    }

    case S2M_UNREGISTER_SLAVE: {
      const Message<S2M_UNREGISTER_SLAVE>& msg = message();

      LOG(INFO) << "Asked to unregister slave " << msg.slave_id();

      // TODO(benh): Check that only the slave is asking to unregister?

      Slave *slave = lookupSlave(msg.slave_id());
      if (slave != NULL)
        removeSlave(slave);
      break;
    }

    case S2M_STATUS_UPDATE: {
      const Message<S2M_STATUS_UPDATE>& msg = message();

      const TaskStatus& status = msg.status();

      LOG(INFO) << "Status update: task " << status.task_id()
		<< " of framework " << msg.framework_id()
		<< " is now in state "
		<< TaskState_descriptor()->FindValueByNumber(status.state())->name();

      Slave* slave = lookupSlave(status.slave_id());
      if (slave != NULL) {
        Framework* framework = lookupFramework(msg.framework_id());
        if (framework != NULL) {
	  // Pass on the (transformed) status update to the framework.
          Message<M2F_STATUS_UPDATE> out;
          out.mutable_framework_id()->MergeFrom(msg.framework_id());
          out.mutable_status()->MergeFrom(status);
          forward(framework->pid, out);

          // No need to reprocess this message if already seen.
          if (duplicate()) {
            LOG(WARNING) << "Ignoring duplicate message with sequence: "
                         << seq();
            break;
          }

          // Update the task state locally.
          Task *task = slave->lookupTask(msg.framework_id(), status.task_id());
          if (task != NULL) {
            task->set_state(status.state());
            // Remove the task if it finished or failed.
            if (status.state() == TASK_FINISHED ||
                status.state() == TASK_FAILED ||
                status.state() == TASK_KILLED ||
                status.state() == TASK_LOST) {
              removeTask(task, TRR_TASK_ENDED);
            }
          } else {
	    LOG(ERROR) << "Status update error: couldn't lookup "
		       << "task " << status.task_id();
	  }
        } else {
          LOG(ERROR) << "Status update error: couldn't lookup "
                     << "framework " << msg.framework_id();
        }
      } else {
        LOG(ERROR) << "Status update error: couldn't lookup slave "
                   << status.slave_id();
      }
      break;
    }

    case S2M_FRAMEWORK_MESSAGE: {
      const Message<S2M_FRAMEWORK_MESSAGE>& msg = message();

      Slave *slave = lookupSlave(msg.message().slave_id());
      if (slave != NULL) {
        Framework *framework = lookupFramework(msg.framework_id());
        if (framework != NULL) {
          Message<M2S_FRAMEWORK_MESSAGE> out;
          out.mutable_framework_id()->MergeFrom(msg.framework_id());
          out.mutable_message()->MergeFrom(msg.message());
          send(framework->pid, out);
        }
      }
      break;
    }

    case S2M_EXITED_EXECUTOR: {
      const Message<S2M_EXITED_EXECUTOR>&msg = message();

      Slave *slave = lookupSlave(msg.slave_id());
      if (slave != NULL) {
        Framework *framework = lookupFramework(msg.framework_id());
        if (framework != NULL) {
          LOG(INFO) << "Executor " << msg.executor_id()
                    << " of framework " << framework->frameworkId
                    << " on slave " << slave->slaveId
                    << " (" << slave->info.hostname() << ") "
                    << "exited with status " << msg.status();

          // Tell the framework which tasks have been lost.
          foreachpaircopy (_, Task* task, framework->tasks) {
            if (task->slave_id() == slave->slaveId &&
                task->executor_id() == msg.executor_id()) {
              Message<M2F_STATUS_UPDATE> out;
              out.mutable_framework_id()->MergeFrom(task->framework_id());
              TaskStatus *status = out.mutable_status();
              status->mutable_task_id()->MergeFrom(task->task_id());
              status->mutable_slave_id()->MergeFrom(task->slave_id());
              status->set_state(TASK_LOST);
              send(framework->pid, out);

              LOG(INFO) << "Removing " << task << " because of lost executor";

              removeTask(task, TRR_EXECUTOR_LOST);
            }
          }

          // TODO(benh): Send the framework it's executor's exit
          // status? Or maybe at least have something like
          // M2F_EXECUTOR_LOST?
        }
      }
      break;
    }

    case SH2M_HEARTBEAT: {
      const Message<SH2M_HEARTBEAT>& msg = message();

      Slave *slave = lookupSlave(msg.slave_id());
      if (slave != NULL) {
        slave->lastHeartbeat = elapsed();
      } else {
        LOG(WARNING) << "Received heartbeat for UNKNOWN slave "
                     << msg.slave_id() << " from " << from();
      }
      break;
    }

    case M2M_TIMER_TICK: {
      foreachpaircopy (_, Slave *slave, slaves) {
	if (slave->lastHeartbeat + HEARTBEAT_TIMEOUT <= elapsed()) {
	  LOG(INFO) << slave
                    << " missing heartbeats ... considering disconnected";
	  removeSlave(slave);
	}
      }

      // Check which framework filters can be expired.
      foreachpair (_, Framework *framework, frameworks)
        framework->removeExpiredFilters(elapsed());

      // Do allocations!
      allocator->timerTick();
      break;
    }

    case M2M_FRAMEWORK_EXPIRED: {
      const Message<M2M_FRAMEWORK_EXPIRED>&msg = message();

      Framework* framework = lookupFramework(msg.framework_id());
      if (framework != NULL) {
	LOG(INFO) << "Framework failover timer expired, removing "
		  << framework;
	removeFramework(framework);
      }
      break;
    }

    case PROCESS_EXIT: {
      // TODO(benh): Could we get PROCESS_EXIT from a network partition?
      LOG(INFO) << "Process exited: " << from();
      if (pidToFrameworkId.count(from()) > 0) {
        const FrameworkID& frameworkId = pidToFrameworkId[from()];
        Framework* framework = lookupFramework(frameworkId);
        if (framework != NULL) {
          LOG(INFO) << framework << " disconnected";

	  // Stop sending offers here for now.
	  framework->active = false;

          // Remove the framework's slot offers.
          foreachcopy (SlotOffer* offer, framework->slotOffers) {
            removeSlotOffer(offer, ORR_FRAMEWORK_FAILOVER, offer->resources);
          }

   	  framework->failoverTimer = new FrameworkFailoverTimer(self(), frameworkId);
   	  link(spawn(framework->failoverTimer));
// 	  removeFramework(framework);
        }
      } else if (pidToSlaveId.count(from()) > 0) {
        const SlaveID& slaveId = pidToSlaveId[from()];
        Slave* slave = lookupSlave(slaveId);
        if (slave != NULL) {
          LOG(INFO) << slave << " disconnected";
          removeSlave(slave);
        }
      } else {
	foreachpair (_, Framework *framework, frameworks) {
	  if (framework->failoverTimer != NULL &&
	      framework->failoverTimer->self() == from()) {
	    LOG(ERROR) << "Bad framework failover timer, removing "
		       << framework;
	    removeFramework(framework);
	    break;
	  }
	}
      }
      break;
    }

    case M2M_GET_STATE: {
      state::MasterState *state = getState();
      Message<M2M_GET_STATE_REPLY> out;
      out.set_pointer((char *) &state, sizeof(state));
      send(from(), out);
      break;
    }
    
    case M2M_SHUTDOWN: {
      LOG(INFO) << "Asked to shut down by " << from();
      foreachpair (_, Slave *slave, slaves)
        send(slave->pid, M2S_SHUTDOWN);
      return;
    }

    default:
      LOG(ERROR) << "Received unknown message (" << msgid()
                 << ") from " << from();
      break;
    }
  }
}


OfferID Master::makeOffer(Framework *framework,
                          const vector<SlaveResources>& resources)
{
  const OfferID& offerId = newOfferId();

  SlotOffer *offer = new SlotOffer(offerId, framework->frameworkId, resources);

  slotOffers[offer->offerId] = offer;
  framework->addOffer(offer);

  // Update the resource information within each of the slave objects. Gross!
  foreach (const SlaveResources& r, resources) {
    r.slave->slotOffers.insert(offer);
    r.slave->resourcesOffered += r.resources;
  }

  LOG(INFO) << "Sending " << offer << " to " << framework;

  Message<M2F_RESOURCE_OFFER> out;
  out.mutable_offer_id()->MergeFrom(offerId);

  foreach (const SlaveResources& r, resources) {
    SlaveOffer* offer = out.add_offer();
    offer->mutable_slave_id()->MergeFrom(r.slave->slaveId);
    offer->set_hostname(r.slave->info.hostname());
    offer->mutable_resources()->MergeFrom(r.resources);

    out.add_pid(r.slave->pid);
  }

  send(framework->pid, out);

  return offerId;
}


// Process a resource offer reply (for a non-cancelled offer) by launching
// the desired tasks (if the offer contains a valid set of tasks) and
// reporting any unused resources to the allocator.
void Master::processOfferReply(SlotOffer* offer,
                               const vector<TaskDescription>& tasks,
                               const Params& params)
{
  LOG(INFO) << "Received reply for " << offer;

  Framework* framework = lookupFramework(offer->frameworkId);
  CHECK(framework != NULL);

  // Count resources in the offer.
  unordered_map<Slave*, Resources> resourcesOffered;
  foreach (const SlaveResources& r, offer->resources) {
    resourcesOffered[r.slave] = r.resources;
  }

  // Count used resources and check that its tasks are valid.
  unordered_map<Slave*, Resources> resourcesUsed;
  foreach (const TaskDescription& task, tasks) {
    // Check whether the task is on a valid slave.
    Slave* slave = lookupSlave(task.slave_id());
    if (slave == NULL || resourcesOffered.count(slave) == 0) {
      terminateFramework(framework, 0, "Invalid slave in offer reply");
      return;
    }

    // Check whether or not the resources for the task are valid.
    // TODO(benh): In the future maybe we can also augment the
    // protobuf to deal with fragmentation purposes by providing some
    // sort of minimum amount of resources required per task.

    if (task.resources().size() == 0) {
      terminateFramework(framework, 0, "Invalid resources for task");
      return;
    }

    foreach (const Resource& resource, task.resources()) {
      if (!Resources::isAllocatable(resource)) {
        // TODO(benh): Also send back the invalid resources as a string?
        terminateFramework(framework, 0, "Invalid resources for task");
        return;
      }
    }

    resourcesUsed[slave] += task.resources();
  }

  // Check that the total accepted on each slave isn't more than offered.
  foreachpair (Slave* slave, const Resources& used, resourcesUsed) {
    if (!(used <= resourcesOffered[slave])) {
      terminateFramework(framework, 0, "Too many resources accepted");
      return;
    }
  }

  // Check that there are no duplicate task IDs.
  unordered_set<TaskID> idsInResponse;
  foreach (const TaskDescription& task, tasks) {
    if (framework->tasks.count(task.task_id()) > 0 ||
        idsInResponse.count(task.task_id()) > 0) {
      terminateFramework(framework, 0, "Duplicate task ID: " +
                         lexical_cast<string>(task.task_id()));
      return;
    }
    idsInResponse.insert(task.task_id());
  }

  // Launch the tasks in the response.
  foreach (const TaskDescription& task, tasks) {
    launchTask(framework, task);
  }

  // Get out the timeout for left over resources (if exists), and use
  // that to calculate the expiry timeout.
  int timeout = DEFAULT_REFUSAL_TIMEOUT;

  for (int i = 0; i < params.param_size(); i++) {
    if (params.param(i).key() == "timeout") {
      timeout = lexical_cast<int>(params.param(i).value());
      break;
    }
  }

  double expiry = (timeout == -1) ? 0 : elapsed() + timeout;  

  // Now check for unused resources on slaves and add filters for them.
  vector<SlaveResources> resourcesUnused;

  foreachpair (Slave* slave, const Resources& offered, resourcesOffered) {
    Resources used = resourcesUsed[slave];
    Resources unused = offered - used;

    CHECK(used == used.allocatable());

    Resources allocatable = unused.allocatable();

    if (allocatable.size() > 0) {
      resourcesUnused.push_back(SlaveResources(slave, allocatable));
    }

    // Only add a filter on a slave if none of the resources are used.
    if (timeout != 0 && used.size() == 0) {
      LOG(INFO) << "Adding filter on " << slave << " to " << framework
                << " for " << timeout << " seconds";
      framework->slaveFilter[slave] = expiry;
    }
  }
  
  // Return the resources left to the allocator.
  removeSlotOffer(offer, ORR_FRAMEWORK_REPLIED, resourcesUnused);
}


void Master::launchTask(Framework* framework, const TaskDescription& task)
{
  // The invariant right now is that launchTask is called only for
  // TaskDescriptions where the slave is still valid (see the code
  // above in processOfferReply).
  Slave *slave = lookupSlave(task.slave_id());
  CHECK(slave != NULL);

  // Determine the executor ID for this task.
  const ExecutorID& executorId = task.has_executor()
    ? task.executor().executor_id()
    : framework->info.executor().executor_id();

  Task *t = new Task();
  t->mutable_framework_id()->MergeFrom(framework->frameworkId);
  t->mutable_executor_id()->MergeFrom(executorId);
  t->set_state(TASK_STARTING);
  t->set_name(task.name());
  t->mutable_task_id()->MergeFrom(task.task_id());
  t->mutable_slave_id()->MergeFrom(task.slave_id());
  t->mutable_resources()->MergeFrom(task.resources());

  framework->addTask(t);
  slave->addTask(t);

  allocator->taskAdded(t);

  LOG(INFO) << "Launching " << t << " on " << slave;

  Message<M2S_RUN_TASK> out;
  out.mutable_framework()->MergeFrom(framework->info);
  out.mutable_framework_id()->MergeFrom(framework->frameworkId);
  out.set_pid(framework->pid);
  out.mutable_task()->MergeFrom(task);
  send(slave->pid, out);
}


void Master::rescindOffer(SlotOffer *offer)
{
  removeSlotOffer(offer, ORR_OFFER_RESCINDED, offer->resources);
}


void Master::killTask(Task *task)
{
  LOG(INFO) << "Killing " << task;

  Framework *framework = lookupFramework(task->framework_id());
  CHECK(framework != NULL);
  Slave *slave = lookupSlave(task->slave_id());
  CHECK(slave != NULL);

  Message<M2S_KILL_TASK> out;
  out.mutable_framework_id()->MergeFrom(framework->frameworkId);
  out.mutable_task_id()->MergeFrom(task->task_id());
  send(slave->pid, out);
}


// Terminate a framework, sending it a particular error message
// TODO: Make the error codes and messages programmer-friendly
void Master::terminateFramework(Framework *framework,
                                int32_t code,
                                const std::string& message)
{
  LOG(INFO) << "Terminating " << framework << " due to error: " << message;

  Message<M2F_ERROR> out;
  out.set_code(code);
  out.set_message(message);
  send(framework->pid, out);

  removeFramework(framework);
}


// Remove a slot offer (because it was replied or we lost a framework or slave)
void Master::removeSlotOffer(SlotOffer *offer,
                             OfferReturnReason reason,
                             const vector<SlaveResources>& resourcesUnused)
{
  // Remove from slaves.
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
    Message<M2F_RESCIND_OFFER> out;
    out.mutable_offer_id()->MergeFrom(offer->offerId);
    send(framework->pid, out);
  }
  
  // Tell the allocator about the unused resources.
  allocator->offerReturned(offer, reason, resourcesUnused);
  
  // Delete it
  slotOffers.erase(offer->offerId);
  delete offer;
}


void Master::addFramework(Framework *framework)
{
  CHECK(frameworks.count(framework->frameworkId) == 0);

  frameworks[framework->frameworkId] = framework;
  pidToFrameworkId[framework->pid] = framework->frameworkId;
  link(framework->pid);

  Message<M2F_REGISTER_REPLY> out;
  out.mutable_framework_id()->MergeFrom(framework->frameworkId);
  send(framework->pid, out);

  allocator->frameworkAdded(framework);
}


// Replace the scheduler for a framework with a new process ID, in the
// event of a scheduler failover.
void Master::failoverFramework(Framework *framework, const PID &newPid)
{
  const PID& oldPid = framework->pid;

  // Remove the framework's slot offers (if they weren't removed before)..
  // TODO(benh): Consider just reoffering these to the new framework.
  foreachcopy (SlotOffer* offer, framework->slotOffers) {
    removeSlotOffer(offer, ORR_FRAMEWORK_FAILOVER, offer->resources);
  }

  Message<M2F_ERROR> out;
  out.set_code(1);
  out.set_message("Framework failover");
  send(oldPid, out);

  // TODO(benh): unlink(old->pid);
  pidToFrameworkId.erase(oldPid);
  pidToFrameworkId[newPid] = framework->frameworkId;

  framework->pid = newPid;
  link(newPid);

  // Kill the failover timer.
  if (framework->failoverTimer != NULL) {
    send(framework->failoverTimer->self(), M2M_SHUTDOWN);
    wait(framework->failoverTimer->self());
    delete framework->failoverTimer;
    framework->failoverTimer = NULL;
  }

  // Make sure we can get offers again.
  framework->active = true;

  Message<M2F_REGISTER_REPLY> reply;
  reply.mutable_framework_id()->MergeFrom(framework->frameworkId);
  send(newPid, reply);
}


// Kill all of a framework's tasks, delete the framework object, and
// reschedule slot offers for slots that were assigned to this framework
void Master::removeFramework(Framework *framework)
{ 
  framework->active = false;
  // TODO: Notify allocator that a framework removal is beginning?
  
  // Tell slaves to kill the framework
  foreachpair (_, Slave *slave, slaves) {
    Message<M2S_KILL_FRAMEWORK> out;
    out.mutable_framework_id()->MergeFrom(framework->frameworkId);
    send(slave->pid, out);
  }

  // Remove pointers to the framework's tasks in slaves
  foreachpaircopy (_, Task *task, framework->tasks) {
    Slave *slave = lookupSlave(task->slave_id());
    CHECK(slave != NULL);
    removeTask(task, TRR_FRAMEWORK_LOST);
  }
  
  // Remove the framework's slot offers (if they weren't removed before).
  foreachcopy (SlotOffer* offer, framework->slotOffers) {
    removeSlotOffer(offer, ORR_FRAMEWORK_LOST, offer->resources);
  }

  // TODO(benh): Similar code between removeFramework and
  // failoverFramework needs to be shared!

  // TODO(benh): unlink(framework->pid);
  pidToFrameworkId.erase(framework->pid);

  // Delete it
  frameworks.erase(framework->frameworkId);
  allocator->frameworkRemoved(framework);
  delete framework;
}


// Lose all of a slave's tasks and delete the slave object
void Master::removeSlave(Slave *slave)
{ 
  slave->active = false;
  // TODO: Notify allocator that a slave removal is beginning?
  
  // Remove pointers to slave's tasks in frameworks, and send status updates
  foreachpaircopy (_, Task *task, slave->tasks) {
    Framework *framework = lookupFramework(task->framework_id());
    // A framework might not actually exist because the master failed
    // over and the framework hasn't reconnected. This can be a tricky
    // situation for frameworks that want to have high-availability,
    // because if they eventually do connect they won't ever get a
    // status update about this task.  Perhaps in the future what we
    // want to do is create a local Framework object to represent that
    // framework until it fails over. See the TODO above in
    // S2M_REREGISTER_SLAVE.
    if (framework != NULL) {
      Message<M2F_STATUS_UPDATE> out;
      out.mutable_framework_id()->MergeFrom(task->framework_id());
      TaskStatus *status = out.mutable_status();
      status->mutable_task_id()->MergeFrom(task->task_id());
      status->mutable_slave_id()->MergeFrom(task->slave_id());
      status->set_state(TASK_LOST);
      send(framework->pid, out);
    }
    removeTask(task, TRR_SLAVE_LOST);
  }

  // Remove slot offers from the slave; this will also rescind them
  foreachcopy (SlotOffer *offer, slave->slotOffers) {
    // Only report resources on slaves other than this one to the allocator
    vector<SlaveResources> otherSlaveResources;
    foreach (const SlaveResources& r, offer->resources) {
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
  foreachpair (_, Framework *framework, frameworks) {
    Message<M2F_LOST_SLAVE> out;
    out.mutable_slave_id()->MergeFrom(slave->slaveId);
    send(framework->pid, out);
  }

  // TODO(benh): unlink(slave->pid);
  pidToSlaveId.erase(slave->pid);

  // Delete it
  slaves.erase(slave->slaveId);
  allocator->slaveRemoved(slave);
  delete slave;
}


// Remove a slot offer (because it was replied or we lost a framework or slave)
void Master::removeTask(Task *task, TaskRemovalReason reason)
{
  Framework *framework = lookupFramework(task->framework_id());
  Slave *slave = lookupSlave(task->slave_id());
  CHECK(framework != NULL);
  CHECK(slave != NULL);
  framework->removeTask(task->task_id());
  slave->removeTask(task);
  allocator->taskRemoved(task, reason);
  delete task;
}


Allocator* Master::createAllocator()
{
  LOG(INFO) << "Creating \"" << allocatorType << "\" allocator";
  return AllocatorFactory::instantiate(allocatorType, this);
}


// Create a new framework ID. We format the ID as MASTERID-FWID, where
// MASTERID is the ID of the master (launch date plus fault tolerant ID)
// and FWID is an increasing integer.
FrameworkID Master::newFrameworkId()
{
  ostringstream oss;
  oss << masterId << "-" << setw(4) << setfill('0') << nextFrameworkId++;
  FrameworkID frameworkId;
  frameworkId.set_value(oss.str());
  return frameworkId;
}


OfferID Master::newOfferId()
{
  OfferID offerId;
  offerId.set_value(masterId + "-" + lexical_cast<string>(nextOfferId++));
  return offerId;
}


SlaveID Master::newSlaveId()
{
  SlaveID slaveId;
  slaveId.set_value(masterId + "-" + lexical_cast<string>(nextSlaveId++));
  return slaveId;
}


const Configuration& Master::getConfiguration()
{
  return conf;
}
