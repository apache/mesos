#ifndef MASTER_STATE_HPP
#define MASTER_STATE_HPP

#include <iostream>
#include <string>
#include <vector>

#include <mesos_types.hpp>

#include "common/foreach.hpp"

#include "config/config.hpp"


namespace mesos { namespace internal { namespace master { namespace state {

struct SlaveResources
{  
  SlaveID slave_id;
  int32_t cpus;
  int64_t mem;
  
  SlaveResources(SlaveID _sid, int32_t _cpus, int64_t _mem)
    : slave_id(_sid), cpus(_cpus), mem(_mem) {}
};


struct SlotOffer
{  
  OfferID id;
  FrameworkID framework_id;
  std::vector<SlaveResources *> resources;
  
  SlotOffer(OfferID _id, FrameworkID _fid)
    : id(_id), framework_id(_fid) {}
    
  ~SlotOffer()
  {
    foreach (SlaveResources *sr, resources)
      delete sr;
  }
};


struct Slave
{
  Slave(SlaveID id_, const std::string& host_, const std::string& web_ui_url_,
	int32_t cpus_, int64_t mem_, time_t connect_)
    : id(id_), host(host_), web_ui_url(web_ui_url_),
      cpus(cpus_), mem(mem_), connect_time(connect_) {}

  Slave() {}

  SlaveID id;
  std::string host;
  std::string web_ui_url;
  int32_t cpus;
  int64_t mem;
  int64_t connect_time;
};


struct Task
{
  Task(TaskID id_, const std::string& name_, FrameworkID fid_, SlaveID sid_,
       TaskState state_, int32_t _cpus, int64_t _mem)
    : id(id_), name(name_), framework_id(fid_), slave_id(sid_), state(state_), 
      cpus(_cpus), mem(_mem) {}

  Task() {}

  TaskID id;
  std::string name;
  FrameworkID framework_id;
  SlaveID slave_id;
  TaskState state;
  int32_t cpus;
  int64_t mem;
};


struct Framework
{
  Framework(FrameworkID id_, const std::string& user_,
      const std::string& name_, const std::string& executor_,
      int32_t cpus_, int64_t mem_, time_t connect_)
    : id(id_), user(user_), name(name_), executor(executor_),
      cpus(cpus_), mem(mem_), connect_time(connect_) {}

  Framework() {}

  ~Framework()
  {
    foreach (Task *task, tasks)
      delete task;
    foreach (SlotOffer *offer, offers)
      delete offer;
  }

  FrameworkID id;
  std::string user;
  std::string name;
  std::string executor;
  int32_t cpus;
  int64_t mem;
  int64_t connect_time;

  std::vector<Task *> tasks;
  std::vector<SlotOffer *> offers;
};


struct MasterState
{
  MasterState(const std::string& build_date_, const std::string& build_user_,
	      const std::string& pid_, bool _isFT = false)
    : build_date(build_date_), build_user(build_user_), pid(pid_), isFT(_isFT) {}

  MasterState() {}

  ~MasterState()
  {
    foreach (Slave *slave, slaves)
      delete slave;
    foreach (Framework *framework, frameworks)
      delete framework;
  }

  std::string build_date;
  std::string build_user;
  std::string pid;

  std::vector<Slave *> slaves;
  std::vector<Framework *> frameworks;
  bool isFT;
};

}}}} /* namespace */

#endif /* MASTER_STATE_HPP */
