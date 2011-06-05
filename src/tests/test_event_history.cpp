#include <cstdio>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include "common/params.hpp"

#include "event_history/event_history.hpp"

#include "master/master.hpp"

#include "testing_utils.hpp"

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
 * preconditions: /tmp is writable by user running this test
 */
TEST_WITH_WORKDIR(EventHistoryTest, EventLoggingTurnedOffWithLogDir)
{
  EXPECT_TRUE(GTEST_IS_THREADSAFE);

  Params params;
  const char* testLogDir = "/tmp/mesos-EventLoggingTurnedOff-test-dir";
  system((string("rm -rf ") + testLogDir).c_str()); // remove tmp dir, if exists
  struct stat sb;
  stat((string(testLogDir) + "/event_history_log.txt").c_str(), &sb);
  EXPECT_FALSE(S_ISREG(sb.st_mode));
  EXPECT_EQ(0, mkdir(testLogDir, 0777)); // create temporary test dir
  params.set<string>("log_dir", testLogDir);
  params.set<bool>("event_history_sqlite", false);
  params.set<bool>("event_history_file", false);
  EventLogger evLogger(params);
  FrameworkID fid = "MasterID-FrameworkID";
  evLogger.logFrameworkRegistered(fid, "UserID");

  // make sure eventlog file and sqlite db were NOT created
  EXPECT_NE(0, stat((string(testLogDir) + "/event_history_log.txt").c_str(), &sb));
  EXPECT_FALSE(S_ISREG(sb.st_mode));

  EXPECT_NE(0, stat((string(testLogDir) + "/event_history_db.sqlite3").c_str(), &sb));
  EXPECT_FALSE(S_ISREG(sb.st_mode));
}


TEST_WITH_WORKDIR(EventHistoryTest, UsesLogDirLocation)
{
  EXPECT_TRUE(GTEST_IS_THREADSAFE);

  Params params;
  const char* testLogDir = "/tmp/mesos-EventLoggingTurnedOff-test-dir";
  system((string("rm -rf ") + testLogDir).c_str()); // remove tmp dir, if exists
  struct stat sb;
  stat((string(testLogDir) + "/event_history_log.txt").c_str(), &sb);
  EXPECT_FALSE(S_ISREG(sb.st_mode));
  EXPECT_EQ(0, mkdir(testLogDir, 0777)); // create temporary test dir
  params.set<string>("log_dir", testLogDir);
  params.set<bool>("event_history_sqlite", true);
  params.set<bool>("event_history_file", true);
  EventLogger evLogger(params);
  FrameworkID fid = "MasterID-FrameworkID";
  evLogger.logFrameworkRegistered(fid, "UserID");

  // make sure eventlog file and sqlite WERE created in the correct spot
  EXPECT_EQ(0, stat((string(testLogDir) + "/event_history_log.txt").c_str(), &sb));
  EXPECT_TRUE(S_ISREG(sb.st_mode));

  EXPECT_EQ(0, stat((string(testLogDir) + "/event_history_db.sqlite3").c_str(), &sb));
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

