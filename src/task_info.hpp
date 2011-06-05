#ifndef _TASK_INFO_HPP_
#define _TASK_INFO_HPP_

#include <string>
#include <glog/logging.h>
#include <nexus_types.hpp>

namespace nexus { namespace internal { 

// An active task. The master only keeps track of tasks that are active.
struct TaskInfo
{ 
  TaskID id;
  FrameworkID frameworkId; // Which framework we belong to
  Resources resources;
  TaskState state;
  string name;
  string message;
  SlaveID slaveId;        // Which slave we're on, not used inside slave.*pp.
  
  TaskInfo() {}
  
  TaskInfo(const TaskID &_id, const Resources &_res, const SlaveID &_sid = "") : 
    id(_id), resources(_res), slaveId(_sid) {}

  TaskInfo(const TaskID &_id, const FrameworkID &_fid, const Resources &_res, 
           const TaskState &_s, const string &_n, const string &_m, const SlaveID &_sid = "") 
    : id(_id), frameworkId(_fid), resources(_res), state(_s), name(_n), 
      message(_m), slaveId(_sid)
  { }
};

}}

#endif /* _TASK_INFO_HPP_ */

