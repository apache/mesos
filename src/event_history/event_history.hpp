#ifndef __EVENT_HISTORY_HPP__
#define __EVENT_HISTORY_HPP__

//#include <fstream>
//#include <iostream>
#include <list>
//#include <map>
#include <string>

#include <glog/logging.h>

#include "common/resources.hpp"

#include "configurator/configurator.hpp"

#include "mesos.hpp"

#include "event_writer.hpp"

namespace mesos { namespace internal { namespace eventhistory {

using namespace std; //for list
using mesos::FrameworkID;
using mesos::TaskID;
using mesos::SlaveID;
using mesos::FrameworkID;
using mesos::TaskState;
using mesos::internal::Resources;


class EventLogger {
private:
  list<EventWriter*> writers; 
public:
  EventLogger();
  EventLogger(const Params&);
  ~EventLogger();
  static void registerOptions(Configurator*);
  int logResourceOffer(FrameworkID, Resources);
  int logTaskCreated(TaskID, FrameworkID, SlaveID, string sHostname, Resources);
  int logTaskStateUpdated(TaskID, FrameworkID, TaskState); 
  int logFrameworkRegistered(FrameworkID, string);
  int logFrameworkUnregistered(FrameworkID);
  EventLogger operator() (string, string);
};

}}} /* namespace */

#endif /* __EVENT_HISTORY_HPP__ */
