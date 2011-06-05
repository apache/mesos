#include <cstdio>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include "common/params.hpp"

#include "event_history/event_logger.hpp"

#include "master/master.hpp"

#include "tests/utils.hpp"

using namespace mesos::internal::eventhistory;
using namespace mesos::internal::test;
using mesos::FrameworkID;
using mesos::internal::Params;

/*  TODO(*):
    1) EventLogger API test
      - create an EventLogger, register a TestEventWriter, test all 
        logging statements. 
    2) Master and Slave's use of Event history
      - create a Master and Slave, simulate sending them messages (e.g.
        adding a slave to a master) and see if the right output is logged
        for each EventWriter type.
 */

/*
 * precondition:  cwd does not contain event_history_log.txt or
 *                event_history_db.sqlite3 files.
 * postcondition: event_history_log.txt and event_history_db.sqlite3
 *                still do not exist (i.e. were not created).
 */
TEST_WITH_WORKDIR(EventHistoryTest, EventLoggingTurnedOffWithLogDir)
{
  EXPECT_TRUE(GTEST_IS_THREADSAFE);

  Params params;
  const char* logDir = "./";
  params.set<string>("log_dir", logDir);
  params.set<bool>("event_history_sqlite", false);
  params.set<bool>("event_history_file", false);
  EventLogger evLogger(params);
  FrameworkID fid = "MasterID-FrameworkID";
  evLogger.logFrameworkRegistered(fid, "UserID");

  // make sure eventlog file and sqlite db were NOT created
  struct stat sb;
  EXPECT_NE(0, stat((string(logDir) + "/event_history_log.txt").c_str(), &sb));
  EXPECT_FALSE(S_ISREG(sb.st_mode));

  EXPECT_NE(0, stat((string(logDir) + "/event_history_db.sqlite3").c_str(), &sb));
  EXPECT_FALSE(S_ISREG(sb.st_mode));
}


/*
 * precondition:  cwd does not contain event_history_log.txt or
 *                event_history_db.sqlite3 files.
 * postcondition: event_history_log.txt and event_history_db.sqlite3
 *                exist (i.e. were created).
 */
TEST_WITH_WORKDIR(EventHistoryTest, UsesLogDirLocation)
{
  EXPECT_TRUE(GTEST_IS_THREADSAFE);

  Params params;
  const char* logDir = "./";
  params.set<string>("log_dir", "./");
  params.set<bool>("event_history_sqlite", true);
  params.set<bool>("event_history_file", true);
  EventLogger evLogger(params);
  FrameworkID fid = "MasterID-FrameworkID";
  evLogger.logFrameworkRegistered(fid, "UserID");

  // make sure eventlog file and sqlite WERE created in the correct spot
  struct stat sb;
  EXPECT_EQ(0, stat((string(logDir) + "/event_history_log.txt").c_str(), &sb));
  EXPECT_TRUE(S_ISREG(sb.st_mode));

  EXPECT_EQ(0, stat((string(logDir) + "/event_history_db.sqlite3").c_str(), &sb));
  EXPECT_TRUE(S_ISREG(sb.st_mode));
}


TEST_WITH_WORKDIR(EventHistoryTest, NoEventLoggingIfLogDirNotSet)
{
  EXPECT_TRUE(GTEST_IS_THREADSAFE);

  Params params;
  EXPECT_FALSE(params.contains("log_dir"));
  params.set<bool>("event_history_sqlite", true);
  params.set<bool>("event_history_file", true);
  EventLogger evLogger(params);
  FrameworkID fid = "MasterID-FrameworkID";
  evLogger.logFrameworkRegistered(fid, "UserID");

  // make sure eventlog file and sqlite db were NOT created in default log dir
  struct stat sb;
  EXPECT_NE(0, stat("logs/event_history_log.txt", &sb));
  EXPECT_FALSE(S_ISREG(sb.st_mode));

  EXPECT_EQ(stat("logs/event_history_db.sqlite3", &sb), -1);
  EXPECT_FALSE(S_ISREG(sb.st_mode));
}

