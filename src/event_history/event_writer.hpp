#ifndef __EVENT_WRITER_HPP__
#define __EVENT_WRITER_HPP__

#include <string>

#include "configurator/configurator.hpp"
#include "common/resources.hpp"

#include "mesos.hpp"

namespace mesos { namespace internal { namespace eventhistory {

using namespace std;

using mesos::FrameworkID;
using mesos::TaskID;
using mesos::SlaveID;
using mesos::FrameworkID;
using mesos::TaskState;
using mesos::internal::Resources;

class EventWriter {
public:
  virtual ~EventWriter() {}
  virtual string getName() = 0;
  virtual int logTaskCreated(TaskID, FrameworkID, SlaveID, 
                             string sHostname, Resources) = 0;
  virtual int logTaskStateUpdated(TaskID, FrameworkID, TaskState) = 0; 
  virtual int logFrameworkRegistered(FrameworkID, string) = 0;
  virtual int logFrameworkUnregistered(FrameworkID) = 0;
};

}}} /* namespace */

#endif /* __EVENT_WRITER_HPP__ */
