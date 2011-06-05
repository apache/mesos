#ifndef __FILE_EVENT_WRITER_HPP__
#define __FILE_EVENT_WRITER_HPP__

#include "configurator/configurator.hpp"

#include "event_writer.hpp"

namespace mesos { namespace internal { namespace eventhistory {

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

}}} /* namespace */

#endif /* __FILE_EVENT_WRITER_HPP__ */
