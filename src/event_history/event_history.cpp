#include <cstdlib>
#include <stdarg.h>
#include <sys/stat.h>

#include <glog/logging.h>

#include "event_history.hpp"
#include "file_event_writer.hpp"
#include "sqlite_event_writer.hpp"

using namespace mesos::internal::eventhistory;


void EventLogger::registerOptions(Configurator* conf) {
  //TODO(andyk): We don't set the default value here, since this would
  //             override the defaults set at the param.get() calls later.
  //             We would like to set the default here and override it later.
  conf->addOption<bool>("event_history_file",
        "Enable file event history logging(default: true)");
  conf->addOption<bool>("event_history_sqlite",
        "Enable SQLite event history logging (default: false)");
}


EventLogger::EventLogger() { }


EventLogger::EventLogger(const Params& conf) {
  struct stat sb;
  string logDir = conf.get("log_dir", "");
  if (logDir != "") {
    LOG(INFO) << "creating EventLogger, using log_dir: " << logDir << endl;
    if (stat(logDir.c_str(), &sb) == -1) {
      LOG(INFO) << "The log directory (" << logDir << ") does not exist, "
                 << "creating it now." << endl ;
      if (mkdir(logDir.c_str(), S_IRWXU | S_IRWXG) != 0) {
        LOG(ERROR) << "encountered an error while creating 'logs' directory, "
                   << "file based event history will not be captured";
      }
    }
    //Create and add file based writers (i.e. writers which depend on log_dir
    //being set) to writers list.
    if (conf.get<bool>("event-history-file", true)) {
      LOG(INFO) << "creating FileEventWriter" << endl;
      writers.push_front(new FileEventWriter(conf));
    }
    if (conf.get<bool>("event-history-sqlite", false)) {
      LOG(INFO) << "creating SqliteEventWriter" << endl;
      writers.push_front(new SqlLiteEventWriter(conf));
    }
  } else {
    LOG(INFO) << "No log directory was specified, so not creating "
              << "FileEventWriter or SqliteEventWriter. No event "
              << "logging will happen!";
    //Create and add non file based writers to writers list here.
  }
}


EventLogger::~EventLogger() {
  //Delete all eventWriters in list.
  list<EventWriter*>::iterator it;
  for (it = writers.begin(); it != writers.end(); it++) {
    delete *it;
  }
}

int EventLogger::logFrameworkRegistered(FrameworkID fwid, string user) {
  list<EventWriter*>::iterator it;
  for (it = writers.begin(); it != writers.end(); it++) {
    (*it)->logFrameworkRegistered(fwid, user);
    DLOG(INFO) << "logged FrameworkRegistered event with " << (*it)->getName()
               << ". fwid: " << fwid << ", user: " << user << endl;
  }
}


int EventLogger::logFrameworkUnregistered(FrameworkID fwid) {
  list<EventWriter*>::iterator it;
  for (it = writers.begin(); it != writers.end(); it++) {
    (*it)->logFrameworkUnregistered(fwid);
    DLOG(INFO) << "logged FrameworkUnregistered event with " << (*it)->getName()
               << ". fwid: " << fwid << endl;
  }
}


int EventLogger::logTaskCreated(TaskID tid, FrameworkID fwid, SlaveID sid,
                                string webuiUrl, Resources resVec)
{
  list<EventWriter*>::iterator it;
  for (it = writers.begin(); it != writers.end(); it++) {
    (*it)->logTaskCreated(tid, fwid, sid, webuiUrl, resVec);
    DLOG(INFO) << "logged TaskCreated event with " << (*it)->getName() << endl;
  }
}


int EventLogger::logTaskStateUpdated(TaskID tid, FrameworkID fwid,
                                     TaskState state)
{
  list<EventWriter*>::iterator it;
  for (it = writers.begin(); it != writers.end(); it++) {
    (*it)->logTaskStateUpdated(tid, fwid, state);
    DLOG(INFO) << "logged TaskStateUpated event with " << (*it)->getName()
               << endl;
  }
}
