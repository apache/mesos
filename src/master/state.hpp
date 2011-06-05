#ifndef MASTER_STATE_HPP
#define MASTER_STATE_HPP

#include <iostream>
#include <string>
#include <vector>

#include "common/foreach.hpp"

#include "config/config.hpp"


// TODO(...): Make all the variable naming in here consistant with the
// rest of the code base. This will require cleaning up some Python code.


namespace mesos { namespace internal { namespace master { namespace state {

struct SlaveResources
{  
  std::string slave_id;
  int32_t cpus;
  int64_t mem;
  
  SlaveResources(std::string _slaveId, int32_t _cpus, int64_t _mem)
    : slave_id(_slaveId), cpus(_cpus), mem(_mem) {}
};


struct SlotOffer
{  
  std::string id;
  std::string framework_id;
  std::vector<SlaveResources *> resources;
  
  SlotOffer(std::string _id, std::string _frameworkId)
    : id(_id), framework_id(_frameworkId) {}
    
  ~SlotOffer()
  {
    foreach (SlaveResources *sr, resources)
      delete sr;
  }
};


struct Slave
{
  Slave(std::string id_, const std::string& host_,
        const std::string& public_dns_,
	int32_t cpus_, int64_t mem_, time_t connect_)
    : id(id_), host(host_), public_dns(public_dns_),
      cpus(cpus_), mem(mem_), connect_time(connect_) {}

  Slave() {}

  std::string id;
  std::string host;
  std::string public_dns;
  int32_t cpus;
  int64_t mem;
  int64_t connect_time;
};


struct Task
{
  Task(std::string id_, const std::string& name_, std::string framework_id_,
       std::string slaveId_, std::string state_, int32_t _cpus, int64_t _mem)
    : id(id_), name(name_), framework_id(framework_id_), slave_id(slaveId_),
      state(state_), cpus(_cpus), mem(_mem) {}

  Task() {}

  std::string id;
  std::string name;
  std::string framework_id;
  std::string slave_id;
  std::string state;
  int32_t cpus;
  int64_t mem;
};


struct Framework
{
  Framework(std::string id_, const std::string& user_,
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

  std::string id;
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
	      const std::string& pid_)
    : build_date(build_date_), build_user(build_user_), pid(pid_) {}

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
};

}}}} /* namespace */

#endif /* MASTER_STATE_HPP */
