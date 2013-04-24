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

#include "master/allocator.hpp"
#include "master/flags.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#include "slave/flags.hpp"
#ifdef __linux__
#include "slave/cgroups_isolator.hpp"
#endif
#include "slave/process_isolator.hpp"
#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using namespace process;

using mesos::internal::master::Allocator;
using mesos::internal::master::HierarchicalDRFAllocatorProcess;
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

// TODO(bmahler): This test is disabled on OSX, until proc::children
// is implemented for OSX.
#ifdef __APPLE__
TYPED_TEST(IsolatorTest, DISABLED_Usage)
#else
TYPED_TEST(IsolatorTest, Usage)
#endif
{
  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  TypeParam isolator;
  Slave s(this->slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status1, status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  driver.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers);

  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("isolator_test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  const std::string& file = path::join(this->slaveFlags.work_dir, "ready");

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

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status1);

  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  // Wait for the task to begin inducing cpu time.
  while (!os::exists(file));

  ExecutorID executorId;
  executorId.set_value(task.task_id().value());

  // We'll wait up to 10 seconds for the child process to induce
  // 1/8 of a second of user and system cpu time in total.
  // TODO(bmahler): Also induce rss memory consumption, by re-using
  // the balloon framework.
  ResourceStatistics statistics;
  Duration waited = Seconds(0);
  do {
    const Future<ResourceStatistics>& usage =
      isolator.usage(frameworkId.get(), executorId);

    AWAIT_READY(usage);

    statistics = usage.get();

    // If we meet our usage expectations, we're done!
    if (statistics.memory_rss() >= 1024u &&
        statistics.cpu_user_time() >= 0.125 &&
        statistics.cpu_system_time() >= 0.125) {
      break;
    }

    const Duration& sleep = Milliseconds(100);
    usleep((useconds_t) sleep.us());
    waited = Seconds(waited.secs() + sleep.secs());
  } while (waited < Seconds(10));

  EXPECT_GE(statistics.memory_rss(), 1024u);
  EXPECT_GE(statistics.cpu_user_time(), 0.125);
  EXPECT_GE(statistics.cpu_system_time(), 0.125);

  driver.killTask(task.task_id());

  AWAIT_READY_FOR(status2, Seconds(5));

  // TODO(bmahler): The command executor is buggy in that it does not
  // send TASK_KILLED for a non-zero exit code due to a kill.
  EXPECT_EQ(TASK_FAILED, status2.get().state());

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}
