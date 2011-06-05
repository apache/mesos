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
        adding a slave to a master) and see if the right output is logged.
    3) EventWriter tests
 */

TEST_WITH_WORKDIR(EventHistoryTest, EventLoggingTurnedOff)
{
  EXPECT_TRUE(GTEST_IS_THREADSAFE);

  Params params;
  params.set<bool>("event-history-sqlite", false);
  params.set<bool>("event-history-file", false);
  EventLogger evLogger(params);
  FrameworkID fid = "MasterID-FrameworkID";
  evLogger.logFrameworkRegistered(fid, "UserID");

  struct stat sb;
  // make sure eventlog file and sqlite db were NOT created
  EXPECT_EQ(stat("logs/event_history_log.txt", &sb), -1);
  EXPECT_FALSE(S_ISREG(sb.st_mode));

  EXPECT_EQ(stat("logs/event_history_db.sqlite3", &sb), -1);
  EXPECT_FALSE(S_ISREG(sb.st_mode));
}

//This test might be unecessary
TEST_WITH_WORKDIR(EventHistoryTest, NoEventLoggingIfLogDirNotSet)
{
  EXPECT_TRUE(GTEST_IS_THREADSAFE);

  struct stat sb;
  //the following two lines might be unecesary 
  remove("logs/event_history_log.txt");
  EXPECT_EQ(stat("logs", &sb), -1);

  Params params;
  params.set<bool>("event-history-sqlite", true);
  params.set<bool>("event-history-file", true);
  params.set<string>("log_dir", "");
  EventLogger evLogger(params);
  FrameworkID fid = "MasterID-FrameworkID";
  evLogger.logFrameworkRegistered(fid, "UserID");

  // make sure eventlog file and sqlite db were NOT created
  EXPECT_EQ(stat("logs/event_history_log.txt", &sb), -1);
  EXPECT_FALSE(S_ISREG(sb.st_mode));

  EXPECT_EQ(stat("logs/event_history_db.sqlite3", &sb), -1);
  EXPECT_FALSE(S_ISREG(sb.st_mode));
}

TEST_WITH_WORKDIR(EventHistoryTest, UsesLogDirLocation)
{
  EXPECT_TRUE(GTEST_IS_THREADSAFE);

  Params params;
  params.set<bool>("event-history-sqlite", true);
  params.set<bool>("event-history-file", true);
  params.set<string>("log_dir", "test-log-dir");
  EventLogger evLogger(params);
  FrameworkID fid = "MasterID-FrameworkID";
  evLogger.logFrameworkRegistered(fid, "UserID");

  struct stat sb;
  // make sure eventlog file and sqlite db were created in the correct location
  EXPECT_NE(stat("test-log-dir/event_history_log.txt", &sb), -1);
  EXPECT_TRUE(S_ISREG(sb.st_mode));

  EXPECT_NE(stat("test-log-dir/event_history_db.sqlite3", &sb), -1);
  EXPECT_TRUE(S_ISREG(sb.st_mode));
}
