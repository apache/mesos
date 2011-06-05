#include <gmock/gmock.h>

#include <mesos_exec.hpp>
#include <mesos_sched.hpp>

#include <boost/lexical_cast.hpp>

#include "common/date_utils.hpp"

#include "local/local.hpp"

#include "master/master.hpp"

#include "slave/isolation_module.hpp"
#include "slave/process_based_isolation_module.hpp"
#include "slave/slave.hpp"

using namespace mesos;
using namespace mesos::internal;

using boost::lexical_cast;

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;
using mesos::internal::slave::Framework;

using std::string;
using std::map;
using std::vector;


/**
 * These tests aren't using gmock right now, but at some point we
 * might move them in that direction.
 */
class FixedResponseScheduler : public Scheduler
{
public:
  vector<TaskDescription> response;
  string errorMessage;
  
  FixedResponseScheduler(vector<TaskDescription> _response)
    : response(_response) {}

  virtual ~FixedResponseScheduler() {}

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo("noexecutor", "");
  }

  virtual void resourceOffer(SchedulerDriver* d,
                             OfferID id,
                             const vector<SlaveOffer>& offers) {
    LOG(INFO) << "FixedResponseScheduler got a slot offer";
    d->replyToOffer(id, response, map<string, string>());
  }
  
  virtual void error(SchedulerDriver* d,
                     int code,
                     const string& message) {
    errorMessage = message;
    d->stop();
  }
};


TEST(MasterTest, DuplicateTaskIdsInResponse)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  DateUtils::setMockDate("200102030405");
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "1";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);
  tasks.push_back(TaskDescription(1, "200102030405-0-0", "", params, ""));
  tasks.push_back(TaskDescription(2, "200102030405-0-0", "", params, ""));
  tasks.push_back(TaskDescription(1, "200102030405-0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  MesosSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Duplicate task ID: 1", sched.errorMessage);
  local::shutdown();
  DateUtils::clearMockDate();
}


TEST(MasterTest, TooMuchMemoryInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  DateUtils::setMockDate("200102030405");
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "1";
  params["mem"] = lexical_cast<string>(4 * Gigabyte);
  tasks.push_back(TaskDescription(1, "200102030405-0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  MesosSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Too many resources accepted", sched.errorMessage);
  local::shutdown();
  DateUtils::clearMockDate();
}


TEST(MasterTest, TooMuchCpuInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  DateUtils::setMockDate("200102030405");
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "4";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);
  tasks.push_back(TaskDescription(1, "200102030405-0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  MesosSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Too many resources accepted", sched.errorMessage);
  local::shutdown();
  DateUtils::clearMockDate();
}


TEST(MasterTest, TooLittleCpuInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  DateUtils::setMockDate("200102030405");
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "0";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);
  tasks.push_back(TaskDescription(1, "200102030405-0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  MesosSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Invalid task size: <0 CPUs, 1024 MEM>", sched.errorMessage);
  local::shutdown();
  DateUtils::clearMockDate();
}


TEST(MasterTest, TooLittleMemoryInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  DateUtils::setMockDate("200102030405");
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "1";
  params["mem"] = "1";
  tasks.push_back(TaskDescription(1, "200102030405-0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  MesosSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Invalid task size: <1 CPUs, 1 MEM>", sched.errorMessage);
  local::shutdown();
  DateUtils::clearMockDate();
}


TEST(MasterTest, TooMuchMemoryAcrossTasks)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  DateUtils::setMockDate("200102030405");
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "1";
  params["mem"] = lexical_cast<string>(2 * Gigabyte);
  tasks.push_back(TaskDescription(1, "200102030405-0-0", "", params, ""));
  tasks.push_back(TaskDescription(2, "200102030405-0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  MesosSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Too many resources accepted", sched.errorMessage);
  local::shutdown();
  DateUtils::clearMockDate();
}


TEST(MasterTest, TooMuchCpuAcrossTasks)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  DateUtils::setMockDate("200102030405");
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "2";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);
  tasks.push_back(TaskDescription(1, "200102030405-0-0", "", params, ""));
  tasks.push_back(TaskDescription(2, "200102030405-0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  MesosSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Too many resources accepted", sched.errorMessage);
  local::shutdown();
  DateUtils::clearMockDate();
}
