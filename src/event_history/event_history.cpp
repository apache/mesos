#include "event_history.hpp"
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <stdarg.h>

#include <glog/logging.h>

using namespace mesos::internal::eventhistory;

//returns the current time in microseconds
long getTimeStamp(){
  struct timeval curr_time;
  struct timezone tzp;
  gettimeofday(&curr_time, &tzp);
  return (long)(curr_time.tv_sec * 1000000 + curr_time.tv_usec);
}


//returns a human readable timestamp
string getHumanReadableTimeStamp(){
  time_t currTime; /* calendar time */
  currTime=time(NULL); /* get current cal time */
  string timestamp = asctime(localtime(&currTime));
  return timestamp.erase(24); /* chop off the newline */
}


//////////FileEventWriter/////////

/* Pre-condition: params["log_dir"] exists and is writable */
FileEventWriter::FileEventWriter(const Params& params) {
  string logDir = params.get("log_dir","");
  CHECK_NE(logDir, "") << "FileEventWriter was created when no log_dir was set";
  logfile.open ((logDir + "/event_history_log.txt").c_str(),ios::app|ios::out);
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
  logfile << getHumanReadableTimeStamp() << ",CreateTask, "
          << "taskid: " << tid << ", "
          << "fwid: " << fwid << ", "
          << "sid: " << sid << ","
          << "cpus: " << resVec.cpus << ", mem: " << resVec.mem << endl;

  return 0;
}


int FileEventWriter::logTaskStateUpdated(TaskID tid, FrameworkID fwid,
                                         TaskState state)
{
  logfile << getHumanReadableTimeStamp() << ", TaskStateUpdate, "
          << "taskid: " << tid << ", "
          << "fwid: " << fwid << ", "
          << "state: " << state << endl;

  return 0;
}


int FileEventWriter::logFrameworkRegistered(FrameworkID fwid, string user) {
  logfile << getHumanReadableTimeStamp() << ", CreateFramework, "
          << "fwid: " << fwid << ", "
          << "userid: " << user << endl;

  return 0;
}


int FileEventWriter::logFrameworkUnregistered(FrameworkID fwid) {
  LOG(FATAL) << "FileEventWriter::logFrameworkUnregistered not implemented yet";
  return -1;
}


//////////SqlLiteEventWriter/////////
static int callback(void *NotUsed, int argc, char **argv, char **azColName){
  int i;
  for(i=0; i<argc; i++){
    printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
  }
  printf("\n");
  return 0;
}


string SqlLiteEventWriter::getName(){
  return "Sqlite Event Writer";
}


/* Pre-condition: params["log_dir"] exists and is writable */
SqlLiteEventWriter::SqlLiteEventWriter(const Params& params) {
  zErrMsg = 0;
  string logDir = params.get("log_dir","");
  CHECK_NE(logDir, "") << "SqlLiteEventWriter constructor failed pre-condition."
                       << " params[\"log_dir\" must be set.";

  //set up log file in log dir
  int rc = sqlite3_open((logDir + "/event_history_db.sqlite3").c_str(),&db);
  if( rc ) {
    LOG(ERROR) << "Can't open database: " << sqlite3_errmsg(db) << endl;
    sqlite3_close(db);
  } else {
    DLOG(INFO) << "opened sql lite db" << endl;
  }
  //create task table in case it doesn't already exist,
  //if it does this shouldn't destroy it
  sqlite3_exec(db, "CREATE TABLE task (taskid Varchar(255), fwid Varchar(255), \
                    sid Varchar(255), webuiUrl Varchar(255), \
                    datetime_created integer, resource_list Varchar(255))",
               ::callback, 0, &zErrMsg);

  sqlite3_exec(db, "CREATE TABLE taskstate (taskid Varchar(255), \
                    fwid Varchar(255), state Varchar(255), \
                    datetime_updated integer)",
               ::callback, 0, &zErrMsg);

  sqlite3_exec(db, "CREATE TABLE framework (fwid Varchar(255), \
                    user Varchar(255), datetime_registered integer)",
               ::callback, 0, &zErrMsg);
}


SqlLiteEventWriter::~SqlLiteEventWriter() {
  sqlite3_close(db);
  DLOG(INFO) << "closed sqllite db" << endl;
}


int SqlLiteEventWriter::logTaskCreated(TaskID tid, FrameworkID fwid,
                                       SlaveID sid, string webuiUrl, 
                                       Resources resVec)
{
  stringstream ss;
  ss << "INSERT INTO task VALUES ("
     << "\"" << tid << "\"" << ","
     << "\"" << fwid << "\"" << ","
     << "\"" << sid << "\"" << ","
     << "\"" << webuiUrl << "\"" << ","
     << getTimeStamp() << ","
     << "'{"
       << "\"cpus\":\"" << resVec.cpus << "\","
       << "\"mem\":\"" << resVec.mem << "\""
     << "}'"
     << ")" << endl;
  DLOG(INFO) << "executing " << ss.str() << endl;
  sqlite3_exec(db, ss.str().c_str(), callback, 0, &zErrMsg);

  return 0;
}


int SqlLiteEventWriter::logTaskStateUpdated(TaskID tid, FrameworkID fwid,
                                            TaskState state)
{
  stringstream ss;
  ss << "INSERT INTO taskstate VALUES ("
     << "\"" << tid << "\"" << ","
     << "\"" << fwid << "\"" << ","
     << "\"" << state << "\"" << ","
     << getTimeStamp() << ")" << endl;
  DLOG(INFO) << "executing " << ss.str() << endl;
  sqlite3_exec(db, ss.str().c_str(), callback, 0, &zErrMsg);

  return 0;
}


int SqlLiteEventWriter::logFrameworkRegistered(FrameworkID fwid, string user) {
  stringstream ss;
  ss << "INSERT INTO framework VALUES ("
     << "\"" << fwid << "\"" << ","
     << "\"" << user << "\"" << ","
     << getTimeStamp() << ")" << endl;
  DLOG(INFO) << "executing " << ss.str() << endl;
  sqlite3_exec(db, ss.str().c_str(), callback, 0, &zErrMsg); 
  return 0;
}


int SqlLiteEventWriter::logFrameworkUnregistered(FrameworkID fwid) {
  LOG(FATAL) << "SqlLiteEvent::logFrameworkUnregistered not implemented yet";
  return -1;
}


/////////////EventLogger//////////
void EventLogger::registerOptions(Configurator* conf) {
  //TODO(andyk): We don't set the default value here, since this would
  //             override the defaults set at the param.get() calls later.
  //             We would like to set the default here and override it later.
  conf->addOption<bool>("event-history-file",
        "Enable file event history logging(default: true)");
  conf->addOption<bool>("event-history-sqlite",
      "Enable SQLite event history logging (default: false)");
}


EventLogger::EventLogger() { }


EventLogger::EventLogger(const Params& params) {
  struct stat sb;
  string logDir = params.get("log_dir", "");
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
    if (params.get<bool>("event-history-file", true)) {
      LOG(INFO) << "creating FileEventWriter" << endl;
      writers.push_front(new FileEventWriter(params));
    }
    if (params.get<bool>("event-history-sqlite", false)) {
      LOG(INFO) << "creating SqliteEventWriter" << endl;
      writers.push_front(new SqlLiteEventWriter(params));
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
