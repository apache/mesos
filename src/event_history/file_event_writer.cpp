#include <fstream>
#include <iostream>

#include <glog/logging.h>

#include "common/date_utils.hpp"

#include "file_event_writer.hpp"

using namespace mesos::internal::eventhistory;

/* Pre-condition: params["log_dir"] exists and is writable */
FileEventWriter::FileEventWriter(const Params& params) {
  string logDir = params.get("log_dir", "");
  CHECK_NE(logDir, "") << "FileEventWriter was created when no log_dir was set";
  logfile.open((logDir + "/event_history_log.txt").c_str(), ios::app|ios::out);
}


string FileEventWriter::getName() {
  return "File Event Writer";
}


FileEventWriter::~FileEventWriter() {
  logfile.close();
  DLOG(INFO) << "closed event log file" << endl;
}


int FileEventWriter::logTaskCreated(TaskID tid, FrameworkID fwid, SlaveID sid,
                                    string webuiUrl, Resources resVec)
{
  logfile << DateUtils::currentDate() << ",CreateTask, "
          << "taskid: " << tid << ", "
          << "fwid: " << fwid << ", "
          << "sid: " << sid << ","
          << "cpus: " << resVec.cpus << ", mem: " << resVec.mem << endl;

  return 0;
}


int FileEventWriter::logTaskStateUpdated(TaskID tid, FrameworkID fwid,
                                         TaskState state)
{
  logfile << DateUtils::currentDate() << ", TaskStateUpdate, "
          << "taskid: " << tid << ", "
          << "fwid: " << fwid << ", "
          << "state: " << state << endl;

  return 0;
}


int FileEventWriter::logFrameworkRegistered(FrameworkID fwid, string user) {
  logfile << DateUtils::currentDate() << ", CreateFramework, "
          << "fwid: " << fwid << ", "
          << "userid: " << user << endl;

  return 0;
}


int FileEventWriter::logFrameworkUnregistered(FrameworkID fwid) {
  LOG(FATAL) << "FileEventWriter::logFrameworkUnregistered not implemented yet";
  return -1;
}
