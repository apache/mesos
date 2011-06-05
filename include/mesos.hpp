#ifndef __MESOS_HPP__
#define __MESOS_HPP__

#include <map>
#include <string>

#include <mesos_types.hpp>


namespace mesos {

// Various Mesos structs that include binary data, such as task descriptions,
// use a std::string to hold it so they can conveniently store the size as
// well as the data in one object. We typedef such strings as bytes
// for two reasons:
// 1) It makes the purpose of fields (human-readable vs opaque) apparent.
// 2) It makes it possible to specify a different typemap for these strings
//    in SWIG code (e.g. to map them to byte[]'s in Java).
typedef std::string bytes;


struct TaskDescription
{
  TaskDescription() {}

  TaskDescription(TaskID _taskId, SlaveID _slaveId, const std::string& _name,
                  const std::map<std::string, std::string>& _params,
                  const bytes& _arg)
    : taskId(_taskId), slaveId(_slaveId), name(_name),
      params(_params), arg(_arg) {}

  TaskID taskId;
  SlaveID slaveId;
  std::string name;
  std::map<std::string, std::string> params;
  bytes arg;
};


struct TaskStatus
{
  TaskStatus() {}

  TaskStatus(TaskID _taskId, TaskState _state, const bytes& _data)
    : taskId(_taskId), state(_state), data(_data) {}

  TaskID taskId;
  TaskState state;
  bytes data;
};


struct SlaveOffer
{
  SlaveOffer() {}

  SlaveOffer(SlaveID _slaveId,
             const std::string& _host,
             const std::map<std::string, std::string>& _params)
    : slaveId(_slaveId), host(_host), params(_params) {}

  SlaveID slaveId;
  std::string host;
  std::map<std::string, std::string> params;
};


struct FrameworkMessage
{
  FrameworkMessage() {}

  FrameworkMessage(SlaveID _slaveId, TaskID _taskId, const bytes& _data)
    : slaveId(_slaveId), taskId(_taskId), data(_data) {}

  SlaveID slaveId;
  TaskID taskId;
  bytes data;
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
  
  ExecutorInfo(const std::string& _uri, const bytes& _initArg)
    : uri(_uri), initArg(_initArg) {}
  
  ExecutorInfo(const std::string& _uri, const bytes& _initArg,
               const std::map<std::string, std::string>& _params)
    : uri(_uri), initArg(_initArg), params(_params) {}

  std::string uri;
  bytes initArg;
  std::map<std::string, std::string> params;
};


}

#endif /* __MESOS_HPP__ */
