#include "messages.hpp"

using std::map;
using std::string;

using namespace process::serialization;


namespace nexus { namespace internal {


void operator & (serializer& s, const TaskState& state)
{
  s & (const int32_t&) state;
}


void operator & (deserializer& s, TaskState& state)
{
  s & (int32_t&) state;
}


void operator & (serializer& s, const SlaveOffer& offer)
{
  s & offer.slaveId;
  s & offer.host;
  s & offer.params;
  s & offer.slavePid;
}


void operator & (deserializer& s, SlaveOffer& offer)
{
  s & offer.slaveId;
  s & offer.host;
  s & offer.params;
  s & offer.slavePid;
}


void operator & (serializer& s, const TaskDescription& task)
{  
  s & task.taskId;
  s & task.slaveId;
  s & task.name;
  s & task.arg;
  s & task.params;
}


void operator & (deserializer& s, TaskDescription& task)
{  
  s & task.taskId;
  s & task.slaveId;
  s & task.name;
  s & task.arg;
  s & task.params;
}


void operator & (serializer& s, const FrameworkMessage& message)
{
  s & message.slaveId;
  s & message.taskId;
  s & message.data;
}


void operator & (deserializer& s, FrameworkMessage& message)
{
  s & message.slaveId;
  s & message.taskId;
  s & message.data;
}


void operator & (serializer& s, const ExecutorInfo& info)
{
  s & info.uri;
  s & info.initArg;
}


void operator & (deserializer& s, ExecutorInfo& info)
{
  s & info.uri;
  s & info.initArg;
}


void operator & (serializer& s, const Params& params)
{
  const map<string, string>& map = params.getMap();
  s & (int32_t) map.size();
  foreachpair (const string& key, const string& value, map) {
    s & key;
    s & value;
  }
}


void operator & (deserializer& s, Params& params)
{
  map<string, string>& map = params.getMap();
  map.clear();
  int32_t size;
  string key;
  string value;
  s & size;
  for (int32_t i = 0; i < size; i++) {
    s & key;
    s & value;
    map[key] = value;
  }
}


void operator & (serializer& s, const Resources& resources)
{
  s & resources.cpus;
  s & resources.mem;
}


void operator & (deserializer& s, Resources& resources)
{
  s & resources.cpus;
  s & resources.mem;
}

void operator & (serializer& s, const Task& taskInfo)
{
  s & taskInfo.id;
  s & taskInfo.frameworkId;
  s & taskInfo.resources;
  s & taskInfo.state;
  s & taskInfo.name;
  s & taskInfo.message;
  s & taskInfo.slaveId;
}

void operator & (deserializer& s, Task& taskInfo)
{
  s & taskInfo.id;
  s & taskInfo.frameworkId;
  s & taskInfo.resources;
  s & taskInfo.state;
  s & taskInfo.name;
  s & taskInfo.message;
  s & taskInfo.slaveId;
}

}} /* namespace nexus { namespace internal { */
