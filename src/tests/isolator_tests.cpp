/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>

#include <gmock/gmock.h>

#include <string>
#include <vector>

#include <process/future.hpp>

#include <stout/os.hpp>
#include <stout/path.hpp>

#include "common/resources.hpp"

#include "detector/detector.hpp"

#include "master/master.hpp"

#include "slave/flags.hpp"
#ifdef __linux__
#include "slave/cgroups_isolator.hpp"
#endif
#include "slave/process_isolator.hpp"
#include "slave/slave.hpp"

#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using namespace process;

using mesos::internal::master::Master;

#ifdef __linux__
using mesos::internal::slave::CgroupsIsolator;
#endif
using mesos::internal::slave::Isolator;
using mesos::internal::slave::ProcessIsolator;
using mesos::internal::slave::Slave;

using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::Return;
using testing::SaveArg;


#ifdef __linux__
typedef ::testing::Types<ProcessIsolator, CgroupsIsolator> IsolatorTypes;
#else
typedef ::testing::Types<ProcessIsolator> IsolatorTypes;
#endif

TYPED_TEST_CASE(IsolatorTest, IsolatorTypes);

TYPED_TEST(IsolatorTest, Usage)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  TypeParam isolator;

  slave::Flags flags = this->CreateSlaveFlags();

  Try<PID<Slave> > slave = this->StartSlave(&isolator, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers);

  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("isolator_test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  Resources resources(offers.get()[0].resources());
  Option<Bytes> mem = resources.mem();
  ASSERT_SOME(mem);
  Option<double> cpus = resources.cpus();
  ASSERT_SOME(cpus);

  const std::string& file = path::join(flags.work_dir, "ready");

  // This task induces user/system load in a child process by
  // running top in a child process for ten seconds.
  task.mutable_command()->set_value(
#ifdef __APPLE__
      // Use logging mode with 30,000 samples with no interval.
      "top -l 30000 -s 0 2>&1 > /dev/null & "
#else
      // Batch mode, with 30,000 samples with no interval.
      "top -b -d 0 -n 30000 2>&1 > /dev/null & "
#endif
      "touch " + file +  "; " // Signals that the top command is running.
      "sleep 60");

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);

  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Wait for the task to begin inducing cpu time.
  while (!os::exists(file));

  ExecutorID executorId;
  executorId.set_value(task.task_id().value());

  // We'll wait up to 10 seconds for the child process to induce
  // 1/8 of a second of user and system cpu time in total.
  // TODO(bmahler): Also induce rss memory consumption, by re-using
  // the balloon framework.
  ResourceStatistics statistics;
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage =
      process::dispatch(
          (Isolator*) &isolator, // TODO(benh): Fix after reaper changes.
          &Isolator::usage,
          frameworkId.get(),
          executorId);

    AWAIT_READY(usage);

    statistics = usage.get();

    // If we meet our usage expectations, we're done!
    if (statistics.cpus_user_time_secs() >= 0.125 &&
        statistics.cpus_system_time_secs() >= 0.125 &&
        statistics.mem_rss_bytes() >= 1024u) {
      break;
    }

    os::sleep(Milliseconds(100));
    waited += Milliseconds(100);
  } while (waited < Seconds(10));


  EXPECT_GE(statistics.cpus_user_time_secs(), 0.125);
  EXPECT_GE(statistics.cpus_system_time_secs(), 0.125);
  EXPECT_EQ(statistics.cpus_limit(), cpus.get());
  EXPECT_GE(statistics.mem_rss_bytes(), 1024u);
  EXPECT_EQ(statistics.mem_limit_bytes(), mem.get().bytes());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.killTask(task.task_id());

  AWAIT_READY(status);

  // TODO(bmahler): The command executor is buggy in that it does not
  // send TASK_KILLED for a non-zero exit code due to a kill.
  EXPECT_EQ(TASK_FAILED, status.get().state());

  driver.stop();
  driver.join();

  this->Shutdown(); // Must shutdown before 'isolator' gets deallocated.
}
