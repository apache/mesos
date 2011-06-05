#include <cstdlib>
#include <stdarg.h>
#include <sys/stat.h>
#include <sstream>

#include <glog/logging.h>

#include "event_logger.hpp"
#include "file_event_writer.hpp"
#include "sqlite_event_writer.hpp"

using namespace mesos::internal::eventhistory;

// We use class data members so that the default values that get 
// printed with --help option match up with those that are used as
// default values when accessing the configuration settings.
bool EventLogger::default_ev_hist_file_conf_val = true;
bool EventLogger::default_ev_hist_sqlite_conf_val = false;

void EventLogger::registerOptions(Configurator* conf, bool file_writer_default,
                                  bool sqlite_writer_default)
{
  default_ev_hist_file_conf_val = file_writer_default;
  default_ev_hist_sqlite_conf_val = sqlite_writer_default;

  ostringstream evFileMessage, evSqliteMessage;
  evFileMessage << "Enable event history file writer (default: "
                << boolalpha << default_ev_hist_file_conf_val << ")";
  evSqliteMessage << "Enable event history sqlite writer (default: "
                  << boolalpha << default_ev_hist_sqlite_conf_val << ")";
  conf->addOption<bool>("event_history_file", evFileMessage.str());
  conf->addOption<bool>("event_history_sqlite", evSqliteMessage.str());
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
    // Create and add file based writers (i.e. writers which depend on log_dir
    // being set) to writers list.
    if (conf.get<bool>("event_history_file", default_ev_hist_file_conf_val)) {
      LOG(INFO) << "creating FileEventWriter" << endl;
      writers.push_front(new FileEventWriter(conf));
    }
    if (conf.get<bool>("event_history_sqlite",
                       default_ev_hist_sqlite_conf_val)) {
      LOG(INFO) << "creating SqliteEventWriter" << endl;
      writers.push_front(new SqlLiteEventWriter(conf));
    }
  } else {
    LOG(INFO) << "No log directory was specified, so not creating "
              << "FileEventWriter or SqliteEventWriter. No event "
              << "logging will happen!";
    // Create and add non file based writers to writers list here.
  }
}


EventLogger::~EventLogger() {
  // Delete all eventWriters in list.
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
