// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <ostream>
#include <set>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/queue.hpp>

#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <mesos/mesos.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/master/detector.hpp>

#include "linux/capabilities.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "tests/cluster.hpp"
#include "tests/mesos.hpp"

#include "tests/containerizer/docker_archive.hpp"

using mesos::internal::capabilities::Capability;
using mesos::internal::capabilities::CHOWN;
using mesos::internal::capabilities::DAC_READ_SEARCH;
using mesos::internal::capabilities::NET_ADMIN;
using mesos::internal::capabilities::NET_RAW;

using mesos::internal::slave::Fetcher;

using mesos::master::detector::MasterDetector;

using process::Future;
using process::Owned;
using process::Queue;

using std::initializer_list;
using std::ostream;
using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

// Param for the tests:
//   'requested'  Framework specified capabilities for the container.
//   'allowed'    Allowed capabilities configured by the operator.
//   'success'    True if the task should finish normally.
struct TestParam
{
  enum Result
  {
    FAILURE = 0,
    SUCCESS = 1
  };

  enum UseImage
  {
    WITHOUT_IMAGE = 0,
    WITH_IMAGE = 1
  };

  TestParam(
      const Option<set<Capability>>& _requested,
      const Option<set<Capability>>& _allowed,
      UseImage _useImage,
      Result _result)
    : requested(_requested.isSome()
        ? convert(_requested.get())
        : Option<CapabilityInfo>::none()),
      allowed(_allowed.isSome()
        ? convert(_allowed.get())
        : Option<CapabilityInfo>::none()),
      useImage(_useImage),
      result(_result) {}

  const Option<CapabilityInfo> requested;
  const Option<CapabilityInfo> allowed;
  const UseImage useImage;
  const Result result;
};


ostream& operator<<(ostream& stream, const TestParam& param)
{
  if (param.requested.isSome()) {
    stream << "requested='" << JSON::protobuf(param.requested.get()) << "', ";
  } else {
    stream << "requested='none', ";
  }

  if (param.allowed.isSome()) {
    stream << "allowed='" << JSON::protobuf(param.allowed.get()) << "', ";
  } else {
    stream << "allowed='none', ";
  }

  switch (param.useImage) {
    case TestParam::WITHOUT_IMAGE:
      stream << "use_image=false, ";
      break;
    case TestParam::WITH_IMAGE:
      stream << "use_image=true, ";
      break;
  }

  switch (param.result) {
    case TestParam::FAILURE:
      stream << "result=failure'";
      break;
    case TestParam::SUCCESS:
      stream << "result=success'";
  }

  return stream;
}


class LinuxCapabilitiesIsolatorTest
  : public MesosTest,
    public ::testing::WithParamInterface<TestParam>
{
public:
  LinuxCapabilitiesIsolatorTest()
    : param(GetParam()) {}

protected:
  TestParam param;
};


ACTION_TEMPLATE(PushTaskStatus,
                HAS_1_TEMPLATE_PARAMS(int, k),
                AND_1_VALUE_PARAMS(statuses))
{
  statuses->put(std::tr1::get<k>(args));
}


// Parameterized test confirming the behavior of the capabilities
// isolator. We here use the fact has `ping` has `NET_RAW` and
// `NET_ADMIN` in its file capabilities. This test should be
// instantiated with above `TestParam` struct.
TEST_P(LinuxCapabilitiesIsolatorTest, ROOT_Ping)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "linux/capabilities";
  flags.allowed_capabilities = param.allowed;

  if (param.useImage == TestParam::WITH_IMAGE) {
    const string registry = path::join(sandbox.get(), "registry");
    AWAIT_READY(DockerArchive::create(registry, "test_image"));

    flags.docker_registry = registry;
    flags.docker_store_dir = path::join(os::getcwd(), "store");
    flags.image_providers = "docker";
    flags.isolation += ",docker/runtime,filesystem/linux";
  }

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

  // We use 'ping' as the command since it has file capabilities
  // (`NET_RAW` and `NET_ADMIN` in permitted set). This allows us to
  // test if capabilities are properly set.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/ping");
  command.add_arguments("ping");
  command.add_arguments("-c");
  command.add_arguments("1");
  command.add_arguments("127.0.0.1");

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      offers.get()[0].resources(),
      command);

  if (param.requested.isSome()) {
    ContainerInfo* container = task.mutable_container();
    container->set_type(ContainerInfo::MESOS);

    LinuxInfo* linux = container->mutable_linux_info();
    CapabilityInfo* capabilities = linux->mutable_capability_info();
    capabilities->CopyFrom(param.requested.get());
  }

  if (param.useImage == TestParam::WITH_IMAGE) {
    ContainerInfo* container = task.mutable_container();
    container->set_type(ContainerInfo::MESOS);

    Image* image = container->mutable_mesos()->mutable_image();
    image->set_type(Image::DOCKER);
    image->mutable_docker()->set_name("test_image");
  }

  Queue<TaskStatus> statuses;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(PushTaskStatus<1>(&statuses));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait for the terminal status update.
  for (;;) {
    Future<TaskStatus> status = statuses.get();
    AWAIT_READY(status);

    TaskState state = status->state();
    if (protobuf::isTerminalState(state)) {
      switch (param.result) {
        case TestParam::SUCCESS:
          EXPECT_EQ(TASK_FINISHED, state);
          break;
        case TestParam::FAILURE:
          EXPECT_EQ(TASK_FAILED, state);
          break;
      }
      break;
    }
  }

  driver.stop();
  driver.join();
}


// TODO(jieyu): We used DAC_READ_SEARCH capability below so that test
// results won't be affected even if the executable (e.g., command
// executor) to launch is not accessible (e.g., under someone's home
// directory). Without that, even ROOT user will receive EACCESS if
// DAC_READ_SEARCH is dropped.
INSTANTIATE_TEST_CASE_P(
    TestParam,
    LinuxCapabilitiesIsolatorTest,
    ::testing::Values(
        // Dropped all relevant capabilities, thus ping will fail.
        TestParam(
            set<Capability>(),
            None(),
            TestParam::WITHOUT_IMAGE,
            TestParam::FAILURE),
        TestParam(
            set<Capability>(),
            None(),
            TestParam::WITH_IMAGE,
            TestParam::FAILURE),
        TestParam(
            set<Capability>({DAC_READ_SEARCH}),
            set<Capability>({NET_RAW, NET_ADMIN, DAC_READ_SEARCH}),
            TestParam::WITHOUT_IMAGE,
            TestParam::FAILURE),
        TestParam(
            set<Capability>({DAC_READ_SEARCH}),
            set<Capability>({NET_RAW, NET_ADMIN, DAC_READ_SEARCH}),
            TestParam::WITH_IMAGE,
            TestParam::FAILURE),
        TestParam(
            set<Capability>({DAC_READ_SEARCH}),
            set<Capability>({CHOWN, DAC_READ_SEARCH}),
            TestParam::WITHOUT_IMAGE,
            TestParam::FAILURE),
        TestParam(
            set<Capability>({DAC_READ_SEARCH}),
            set<Capability>({CHOWN, DAC_READ_SEARCH}),
            TestParam::WITH_IMAGE,
            TestParam::FAILURE),

        // Allowed capabilities do not contain that ping needs, thus
        // ping will fail.
        TestParam(
            None(),
            set<Capability>({CHOWN, DAC_READ_SEARCH}),
            TestParam::WITHOUT_IMAGE,
            TestParam::FAILURE),
        TestParam(
            None(),
            set<Capability>({CHOWN, DAC_READ_SEARCH}),
            TestParam::WITH_IMAGE,
            TestParam::FAILURE),

        // Requested capabilities are not allowed, task will fail.
        TestParam(
            set<Capability>({NET_RAW, NET_ADMIN}),
            set<Capability>({CHOWN}),
            TestParam::WITHOUT_IMAGE,
            TestParam::FAILURE),
        TestParam(
            set<Capability>({NET_RAW, NET_ADMIN}),
            set<Capability>({CHOWN}),
            TestParam::WITH_IMAGE,
            TestParam::FAILURE),

        // Dropped all capabilities but those that ping needs, thus
        // ping will finish normally.
        TestParam(
            set<Capability>({NET_RAW, NET_ADMIN, DAC_READ_SEARCH}),
            None(),
            TestParam::WITHOUT_IMAGE,
            TestParam::SUCCESS),
        TestParam(
            set<Capability>({NET_RAW, NET_ADMIN, DAC_READ_SEARCH}),
            None(),
            TestParam::WITH_IMAGE,
            TestParam::SUCCESS),
        TestParam(
            None(),
            set<Capability>({NET_RAW, NET_ADMIN, DAC_READ_SEARCH}),
            TestParam::WITHOUT_IMAGE,
            TestParam::SUCCESS),
        TestParam(
            None(),
            set<Capability>({NET_RAW, NET_ADMIN, DAC_READ_SEARCH}),
            TestParam::WITH_IMAGE,
            TestParam::SUCCESS),
        TestParam(
            set<Capability>({NET_RAW, NET_ADMIN, DAC_READ_SEARCH}),
            set<Capability>({NET_RAW, NET_ADMIN, DAC_READ_SEARCH}),
            TestParam::WITHOUT_IMAGE,
            TestParam::SUCCESS),
        TestParam(
            set<Capability>({NET_RAW, NET_ADMIN, DAC_READ_SEARCH}),
            set<Capability>({NET_RAW, NET_ADMIN, DAC_READ_SEARCH}),
            TestParam::WITH_IMAGE,
            TestParam::SUCCESS)));


// TODO(bbannier): Add test cases for running the container as non-root.


// TODO(bbannier): Reject these tasks that specify capabilities if
// capabilities isolator is not enabled.

} // namespace tests {
} // namespace internal {
} // namespace mesos {
