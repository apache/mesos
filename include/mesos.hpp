#ifndef __MESOS_HPP__
#define __MESOS_HPP__

#include <map>
#include <string>
#include <process.hpp>
#include <mesos_types.hpp>

namespace mesos {

// Various Mesos structs that include binary data, such as task descriptions,
// use a std::string to hold it so they can conveniently store the size as
// well as the data in one object. We typedef such strings as data_strings
// for two reasons:
// 1) It makes the purpose of fields (human-readable vs opaque) apparent.
// 2) It makes it possible to specify a different typemap for these strings
//    in SWIG code (e.g. to map them to byte[]'s in Java).
typedef std::string data_string;


// Convenience typedef for map<string, string>, which is used for
// key-value parameters throughout the Mesos API
typedef std::map<std::string, std::string> string_map;


struct TaskDescription
{
  TaskDescription() {}

  TaskDescription(TaskID _taskId, SlaveID _slaveId, const std::string& _name,
      const string_map& _params, const data_string& _arg)
    : taskId(_taskId), slaveId(_slaveId), name(_name),
      params(_params), arg(_arg) {}

  TaskID taskId;
  SlaveID slaveId;
  std::string name;
  string_map params;
  data_string arg;
};


struct TaskStatus
{
  TaskStatus() {}

  TaskStatus(TaskID _taskId, TaskState _state, const data_string& _data)
    : taskId(_taskId), state(_state), data(_data) {}

  TaskID taskId;
  TaskState state;
  data_string data;
};


struct SlaveOffer
{
  SlaveOffer() {}

  SlaveOffer(SlaveID _slaveId,
             const std::string& _host,
             const string_map& _params,
             const PID& _slavePid)
    : slaveId(_slaveId), host(_host), params(_params), slavePid(_slavePid) {}

  SlaveID slaveId;
  std::string host;
  string_map params;
  PID slavePid;
};


struct FrameworkMessage
{
  FrameworkMessage() {}

  FrameworkMessage(SlaveID _slaveId, TaskID _taskId, const data_string& _data)
    : slaveId(_slaveId), taskId(_taskId), data(_data) {}

  SlaveID slaveId;
  TaskID taskId;
  data_string data;
};


/**
 * Information used to launch an executor for a framework.
 * This contains an URI to the executor, which may be either an absolute path
 * on a shared file system or a hdfs:// URI, as well as an opaque initArg
 * passed to the executor's init() callback.
 * In addition, for both local and HDFS executor URIs, Mesos supports packing
 * up multiple files in a .tgz. In this case, the .tgz should contain a single
 * directory (with any name) and there should be a script in this directory
 * called "executor" that will launch the executor.
 */
struct ExecutorInfo
{
  ExecutorInfo() {}
  
  ExecutorInfo(const std::string& _uri, const data_string& _initArg)
    : uri(_uri), initArg(_initArg) {}
  
  ExecutorInfo(const std::string& _uri, const data_string& _initArg,
      const string_map& _params)
    : uri(_uri), initArg(_initArg), params(_params) {}

  std::string uri;
  data_string initArg;
  string_map params;
};


}

#endif /* __MESOS_HPP__ */
