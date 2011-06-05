#ifndef __EVENT_HISTORY_HPP__
#define __EVENT_HISTORY_HPP__

#include <ctime>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <string>

#include <sqlite3.h>

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
  virtual int logTaskCreated(TaskID, FrameworkID, SlaveID, string sHostname, Resources) = 0;
  virtual int logTaskStateUpdated(TaskID, FrameworkID, TaskState) = 0; 
  virtual int logFrameworkRegistered(FrameworkID, string) = 0;
  virtual int logFrameworkUnregistered(FrameworkID) = 0;
};


class FileEventWriter : public EventWriter {
private:
  ofstream logfile;
  time_t currTime;
public:
  string getName();
  FileEventWriter(); 
  FileEventWriter(const Params&);
  ~FileEventWriter();
  int logTaskCreated(TaskID, FrameworkID, SlaveID, string sHostname, Resources);
  int logTaskStateUpdated(TaskID, FrameworkID, TaskState); 
  int logFrameworkRegistered(FrameworkID, string);
  int logFrameworkUnregistered(FrameworkID);
};


class SqlLiteEventWriter : public EventWriter {
private:
  sqlite3 *db; 
  char *zErrMsg;
  time_t currTime;
public:
  string getName();
  SqlLiteEventWriter(); 
  SqlLiteEventWriter(const Params&);
  ~SqlLiteEventWriter();
  int logTaskCreated(TaskID, FrameworkID, SlaveID, string sHostname, Resources);
  int logTaskStateUpdated(TaskID, FrameworkID, TaskState); 
  int logFrameworkRegistered(FrameworkID, string);
  int logFrameworkUnregistered(FrameworkID);
};


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
