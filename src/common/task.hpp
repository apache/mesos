#ifndef _TASK_HPP_
#define _TASK_HPP_

#include <string>

#include <mesos_types.hpp>


namespace mesos { namespace internal { 

// An active task. The master only keeps track of tasks that are active.
struct Task
{ 
  TaskID id;
  FrameworkID frameworkId; // Which framework we belong to
  Resources resources;
  TaskState state;
  std::string name;
  std::string message;
  SlaveID slaveId;        // Which slave we're on, not used inside slave.*pp.
  
  Task() {}
  
  Task(const TaskID &_id, const Resources &_res, const SlaveID &_sid = "") : 
    id(_id), resources(_res), slaveId(_sid) {}

  Task(const TaskID &_id, const FrameworkID &_fid, const Resources &_res, 
       const TaskState &_s, const std::string &_n, const std::string &_m,
       const SlaveID &_sid = "") 
    : id(_id), frameworkId(_fid), resources(_res), state(_s), name(_n), 
      message(_m), slaveId(_sid)
  { }
};

}}

#endif /* _TASK_HPP_ */

