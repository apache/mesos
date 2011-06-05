#ifndef __EVENT_HISTORY_HPP__
#define __EVENT_HISTORY_HPP__

#include <list>
#include <string>

#include <glog/logging.h>

#include "common/resources.hpp"

#include "configurator/configurator.hpp"

#include "mesos.hpp"

#include "event_writer.hpp"

namespace mesos { namespace internal { namespace eventhistory {

using namespace std; // For list.
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
  static bool default_ev_hist_file_conf_val;
  static bool default_ev_hist_sqlite_conf_val;

  EventLogger();
  EventLogger(const Params&);
  ~EventLogger();
  static void registerOptions(Configurator*, bool file_writer_default = true,
                                             bool sqlite_writer_default = false);
  int logResourceOffer(FrameworkID, Resources);
  int logTaskCreated(TaskID, FrameworkID, SlaveID, string sHostname, Resources);
  int logTaskStateUpdated(TaskID, FrameworkID, TaskState); 
  int logFrameworkRegistered(FrameworkID, string);
  int logFrameworkUnregistered(FrameworkID);
};

}}} /* namespace */

#endif /* __EVENT_HISTORY_HPP__ */
