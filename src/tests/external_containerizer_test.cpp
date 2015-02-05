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
#include <map>

#include <mesos/resources.hpp>

#include <process/future.hpp>

#include <stout/os.hpp>
#include <stout/path.hpp>

#include "master/master.hpp"
#include "master/detector.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/external_containerizer.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "tests/mesos.hpp"
#include "tests/flags.hpp"

using namespace mesos;
using namespace mesos::tests;

using namespace process;

using mesos::master::Master;
using mesos::slave::Containerizer;
using mesos::slave::Slave;

using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::Return;
using testing::SaveArg;
using testing::Invoke;

// The external containerizer tests currently rely on a Python script
// which needs the Mesos Python egg being built.
// TODO(tillt): Consider providing tests that do not rely on Python.
#ifdef MESOS_HAS_PYTHON

// TODO(tillt): Update and enhance the ExternalContainerizer tests,
// possibly following some of the patterns used within the
// IsolatorTests or even entirely reusing the Containerizer tests.
class ExternalContainerizerTest : public MesosTest {};


class MockExternalContainerizer : public slave::ExternalContainerizer
{
public:
  MOCK_METHOD8(
      launch,
      process::Future<bool>(
          const ContainerID&,
          const TaskInfo&,
          const ExecutorInfo&,
          const std::string&,
          const Option<std::string>&,
          const SlaveID&,
          const process::PID<slave::Slave>&,
          bool checkpoint));

  MockExternalContainerizer(const slave::Flags& flags)
    : ExternalContainerizer(flags)
  {
    // Set up defaults for mocked methods.
    // NOTE: See TestContainerizer::setup for why we use
    // 'EXPECT_CALL' and 'WillRepeatedly' here instead of
    // 'ON_CALL' and 'WillByDefault'.
    EXPECT_CALL(*this, launch(_, _, _, _, _, _, _, _))
      .WillRepeatedly(Invoke(this, &MockExternalContainerizer::_launch));
  }

  process::Future<bool> _launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint)
  {
    return slave::ExternalContainerizer::launch(
        containerId,
        taskInfo,
        executorInfo,
        directory,
        user,
        slaveId,
        slavePid,
        checkpoint);
  }
};


// This test has been temporarily disabled due to MESOS-1257.
TEST_F(ExternalContainerizerTest, DISABLED_Launch)
{
  Try<PID<Master> > master = this->StartMaster();
  ASSERT_SOME(master);

  Flags testFlags;

  slave::Flags flags = this->CreateSlaveFlags();

  flags.isolation = "external";
  flags.containerizer_path =
    testFlags.build_dir + "/src/examples/python/test-containerizer";

  MockExternalContainerizer containerizer(flags);

  Try<PID<Slave> > slave = this->StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

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
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());

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
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore rest for now.

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockExternalContainerizer::_launch)));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(containerId);

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
    Future<ResourceStatistics> usage = containerizer.usage(containerId.get());
    AWAIT_READY(usage);

    statistics = usage.get();

    // If we meet our usage expectations, we're done!
    // NOTE: We are currently getting dummy-data from the test-
    // containerizer python script matching these expectations.
    // TODO(tillt): Consider working with real data.
    if (statistics.cpus_user_time_secs() >= 0.120 &&
        statistics.cpus_system_time_secs() >= 0.05 &&
        statistics.mem_rss_bytes() >= 1024u) {
      break;
    }

    os::sleep(Milliseconds(100));
    waited += Milliseconds(100);
  } while (waited < Seconds(10));

  EXPECT_GE(statistics.cpus_user_time_secs(), 0.120);
  EXPECT_GE(statistics.cpus_system_time_secs(), 0.05);
  EXPECT_EQ(statistics.cpus_limit(), cpus.get());
  EXPECT_GE(statistics.mem_rss_bytes(), 1024u);
  EXPECT_EQ(statistics.mem_limit_bytes(), mem.get().bytes());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.killTask(task.task_id());

  AWAIT_READY(status);

  EXPECT_EQ(TASK_KILLED, status.get().state());

  driver.stop();
  driver.join();

  this->Shutdown();
}

#endif // MESOS_HAS_PYTHON
