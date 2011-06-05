#ifndef __INTERNALINFO_HPP__
#define __INTERNALINFO_HPP__

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
  
  TaskInfo(TaskID _id, Resources _res, SlaveID _sid="") : id(_id), resources(_res), slaveId(_sid) {}

  TaskInfo(TaskID _id, FrameworkID _fid, Resources _res, TaskState _s, string _n, 
	   string _m, SlaveID _sid="") 
    : id(_id), frameworkId(_fid), resources(_res), state(_s), name(_n), 
      message(_m), slaveId(_sid)
  { }
};

}}

#endif /* __INTERNALINFO_HPP__ */

