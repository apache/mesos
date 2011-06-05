#include "sqlite_event_writer.hpp"

using namespace mesos::internal::eventhistory;

static int callback(void *NotUsed, int argc, char **argv, char **azColName) {
  int i;
  for(i=0; i<argc; i++) {
    printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
  }
  printf("\n");
  return 0;
}


string SqlLiteEventWriter::getName() {
  return "Sqlite Event Writer";
}


/* Pre-condition: conf["log_dir"] exists and is writable */
SqlLiteEventWriter::SqlLiteEventWriter(const Params& conf) {
  string logDir = conf.get("log_dir", "");
  CHECK_NE(logDir, "") << "SqlLiteEventWriter constructor failed pre-condition."
                       << " conf[\"log_dir\" must be set.";

  //set up log file in log dir
  int rc = sqlite3_open((logDir + "/event_history_db.sqlite3").c_str(), &db);
  if( rc ) {
    LOG(ERROR) << "Can't open database: " << sqlite3_errmsg(db) << endl;
    sqlite3_close(db);
  } else {
    DLOG(INFO) << "opened sql lite db" << endl;
  }
  //create task table in case it doesn't already exist,
  //if it does this shouldn't destroy it
  char *errMsg = 0;
  sqlite3_exec(db, "CREATE TABLE task (taskid Varchar(255), fwid Varchar(255), \
                    sid Varchar(255), webuiUrl Varchar(255), \
                    datetime_created integer, resource_list Varchar(255))",
               ::callback, 0, &errMsg);

  sqlite3_exec(db, "CREATE TABLE taskstate (taskid Varchar(255), \
                    fwid Varchar(255), state Varchar(255), \
                    datetime_updated integer)",
               ::callback, 0, &errMsg);

  sqlite3_exec(db, "CREATE TABLE framework (fwid Varchar(255), \
                    user Varchar(255), datetime_registered integer)",
               ::callback, 0, &errMsg);
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
     << DateUtils::currentDateTimeInMicro() << ","
     << "'{"
       << "\"cpus\":\"" << resVec.cpus << "\","
       << "\"mem\":\"" << resVec.mem << "\""
     << "}'"
     << ")" << endl;
  DLOG(INFO) << "executing " << ss.str() << endl;
  char *errMsg = 0;
  sqlite3_exec(db, ss.str().c_str(), callback, 0, &errMsg);

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
     << DateUtils::currentDateTimeInMicro() << ")" << endl;
  DLOG(INFO) << "executing " << ss.str() << endl;
  char *errMsg = 0;
  sqlite3_exec(db, ss.str().c_str(), callback, 0, &errMsg);

  return 0;
}


int SqlLiteEventWriter::logFrameworkRegistered(FrameworkID fwid, string user) {
  stringstream ss;
  ss << "INSERT INTO framework VALUES ("
     << "\"" << fwid << "\"" << ","
     << "\"" << user << "\"" << ","
     << DateUtils::currentDateTimeInMicro() << ")" << endl;
  DLOG(INFO) << "executing " << ss.str() << endl;
  sqlite3_exec(db, ss.str().c_str(), callback, 0, &zErrMsg); 
  return 0;
}


int SqlLiteEventWriter::logFrameworkUnregistered(FrameworkID fwid) {
  LOG(FATAL) << "SqlLiteEvent::logFrameworkUnregistered not implemented yet";
  return -1;
}
