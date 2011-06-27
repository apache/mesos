#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include "common/date_utils.hpp"

#include "local/local.hpp"

#include "master/master.hpp"

#include "slave/isolation_module.hpp"
#include "slave/process_based_isolation_module.hpp"
#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;
using mesos::internal::slave::Framework;

using process::PID;

using std::string;
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
  
  FixedResponseScheduler(const vector<TaskDescription>& _response)
    : response(_response) {}

  virtual ~FixedResponseScheduler() {}

  virtual string getFrameworkName(SchedulerDriver*)
  {
    return "Fixed Response Framework";
  }

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    // TODO(benh): The following line crashes some Linux compilers. :(
    // return DEFAULT_EXECUTOR_INFO;
    ExecutorInfo executor;
    executor.mutable_executor_id()->set_value("default");
    executor.set_uri("noexecutor");
    return executor;
  }

  virtual void registered(SchedulerDriver*, const FrameworkID&) {}

  virtual void resourceOffer(SchedulerDriver* driver,
                             const OfferID& offerId,
                             const vector<SlaveOffer>& offers) {
    LOG(INFO) << "FixedResponseScheduler got a slot offer";

    driver->replyToOffer(offerId, response);
  }

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId) {}

  virtual void statusUpdate(SchedulerDriver* driver,
                            const TaskStatus& status) {}

  virtual void frameworkMessage(SchedulerDriver* driver,
				const SlaveID& slaveId,
				const ExecutorID& executorId,
                                const string& data) {}

  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& sid) {}

  virtual void error(SchedulerDriver* driver, int code, const string& message) {
    errorMessage = message;
    driver->stop();
  }
};


TEST(MasterTest, DuplicateTaskIdsInResponse)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DateUtils::setMockDate("200102030405");
  PID<Master> master = local::launch(1, 3, 3 * Gigabyte, false, false);

  Resources resources;

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(1);

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Resource::SCALAR);
  mem.mutable_scalar()->set_value(1 * Gigabyte);

  resources += cpus;
  resources += mem;

  vector<TaskDescription> tasks;

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->set_value("200102030405-0-0");
  task.mutable_resources()->MergeFrom(resources);

  tasks.push_back(task);
  tasks.push_back(task);

  task.mutable_task_id()->set_value("2");

  tasks.push_back(task);

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
  PID<Master> master = local::launch(1, 3, 3 * Gigabyte, false, false);

  Resources resources;

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(1);

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Resource::SCALAR);
  mem.mutable_scalar()->set_value(4 * Gigabyte);

  resources += cpus;
  resources += mem;

  vector<TaskDescription> tasks;

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->set_value("200102030405-0-0");
  task.mutable_resources()->MergeFrom(resources);

  tasks.push_back(task);

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
  PID<Master> master = local::launch(1, 3, 3 * Gigabyte, false, false);

  Resources resources;

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(4);

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Resource::SCALAR);
  mem.mutable_scalar()->set_value(1 * Gigabyte);

  resources += cpus;
  resources += mem;

  vector<TaskDescription> tasks;

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->set_value("200102030405-0-0");
  task.mutable_resources()->MergeFrom(resources);

  tasks.push_back(task);

  FixedResponseScheduler sched(tasks);
  MesosSchedulerDriver driver(&sched, master);

  driver.run();

  EXPECT_EQ("Too many resources accepted", sched.errorMessage);

  local::shutdown();
  DateUtils::clearMockDate();
}


TEST(MasterTest, ZeroCpuInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DateUtils::setMockDate("200102030405");
  PID<Master> master = local::launch(1, 3, 3 * Gigabyte, false, false);

  Resources resources;

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(0);

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Resource::SCALAR);
  mem.mutable_scalar()->set_value(1 * Gigabyte);

  resources += cpus;
  resources += mem;

  vector<TaskDescription> tasks;

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->set_value("200102030405-0-0");
  task.mutable_resources()->MergeFrom(resources);

  tasks.push_back(task);

  FixedResponseScheduler sched(tasks);
  MesosSchedulerDriver driver(&sched, master);

  driver.run();

  EXPECT_EQ("Invalid resources for task", sched.errorMessage);

  local::shutdown();
  DateUtils::clearMockDate();
}


TEST(MasterTest, TooMuchMemoryAcrossTasks)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DateUtils::setMockDate("200102030405");
  PID<Master> master = local::launch(1, 3, 3 * Gigabyte, false, false);

  Resources resources;

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(1);

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Resource::SCALAR);
  mem.mutable_scalar()->set_value(2 * Gigabyte);

  resources += cpus;
  resources += mem;

  vector<TaskDescription> tasks;

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->set_value("200102030405-0-0");
  task.mutable_resources()->MergeFrom(resources);

  tasks.push_back(task);

  task.mutable_task_id()->set_value("2");

  tasks.push_back(task);

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
  PID<Master> master = local::launch(1, 3, 3 * Gigabyte, false, false);

  Resources resources;

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Resource::SCALAR);
  mem.mutable_scalar()->set_value(1 * Gigabyte);

  resources += cpus;
  resources += mem;

  vector<TaskDescription> tasks;

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->set_value("200102030405-0-0");
  task.mutable_resources()->MergeFrom(resources);

  tasks.push_back(task);

  task.mutable_task_id()->set_value("2");

  tasks.push_back(task);

  FixedResponseScheduler sched(tasks);
  MesosSchedulerDriver driver(&sched, master);

  driver.run();

  EXPECT_EQ("Too many resources accepted", sched.errorMessage);

  local::shutdown();
  DateUtils::clearMockDate();
}
