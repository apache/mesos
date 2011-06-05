#ifndef __SQL_EVENT_WRITER_HPP__
#define __SQL_EVENT_WRITER_HPP__

#include <sqlite3.h>

#include "common/date_utils.hpp"

#include "event_writer.hpp"

namespace mesos { namespace internal { namespace eventhistory {

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

}}} /* namespace */

#endif /* __SQL_EVENT_WRITER_HPP__ */
