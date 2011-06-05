#ifndef SLAVE_STATE_HPP
#define SLAVE_STATE_HPP

#include <iostream>
#include <string>
#include <vector>

#include "config.hpp"
#include "foreach.hpp"
#include "messages.hpp"

namespace mesos { namespace internal { namespace slave { namespace state {

struct Task
{
  Task(TaskID id_, const std::string& name_, TaskState state_,
      int32_t cpus_, int64_t mem_)
    : id(id_), name(name_), state(state_), cpus(cpus_), mem(mem_) {}

  Task() {}

  TaskID id;
  std::string name;
  TaskState state;
  int32_t cpus;
  int64_t mem;
};

struct Framework
{
  Framework(FrameworkID id_, const std::string& name_,
      const std::string& executor_uri_, const std::string& executor_status_,
      int32_t cpus_, int64_t mem_)
    : id(id_), name(name_), executor_uri(executor_uri_),
      executor_status(executor_status_), cpus(cpus_), mem(mem_) {}

  Framework() {}

  ~Framework()
  {
    foreach (Task *task, tasks)
      delete task;
  }

  FrameworkID id;
  std::string name;
  std::string executor_uri;
  std::string executor_status;
  int32_t cpus;
  int64_t mem;

  std::vector<Task *> tasks;
};

struct SlaveState
{
  SlaveState(const std::string& build_date_, const std::string& build_user_,
	     SlaveID id_, int32_t cpus_, int64_t mem_, const std::string& pid_,
	     const std::string& master_pid_)
    : build_date(build_date_), build_user(build_user_), id(id_),
      cpus(cpus_), mem(mem_), pid(pid_), master_pid(master_pid_) {}

  SlaveState() {}

  ~SlaveState()
  {
    foreach (Framework *framework, frameworks)
      delete framework;
  }

  std::string build_date;
  std::string build_user;
  SlaveID id;
  int32_t cpus;
  int64_t mem;
  std::string pid;
  std::string master_pid;

  std::vector<Framework *> frameworks;
};

}}}} /* namespace */

#endif /* SLAVE_STATE_HPP */
