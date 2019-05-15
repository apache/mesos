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

#include <set>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/master/detector.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/jsonify.hpp>
#include <stout/os/exists.hpp>

#include "common/protobuf_utils.hpp"

#include "docker/docker.hpp"

#include "master/master.hpp"

#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "slave/containerizer/mesos/isolators/gpu/nvidia.hpp"

#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Gpu;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::NvidiaGpuAllocator;
using mesos::internal::slave::NvidiaVolume;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using process::Future;
using process::Owned;

using std::set;
using std::string;
using std::vector;

using testing::_;
using testing::AllOf;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Return;
using testing::Truly;

namespace mesos {
namespace internal {
namespace tests {

class NvidiaGpuTest : public ContainerizerTest<slave::MesosContainerizer> {};


// This test verifies that we are able to enable the Nvidia GPU
// isolator and launch tasks with restricted access to GPUs. We
// first launch a task with access to 0 GPUs and verify that a
// call to `nvidia-smi` fails. We then launch a task with 1 GPU
// and verify that a call to `nvidia-smi` both succeeds and
// reports exactly 1 GPU available.
TEST_F(NvidiaGpuTest, ROOT_CGROUPS_NVIDIA_GPU_VerifyDeviceAccess)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Turn on Nvidia GPU isolation.
  // Assume at least one GPU is available for isolation.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,cgroups/devices,gpu/nvidia";
  flags.resources = "cpus:1"; // To override the default with gpus:0.

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::GPU_RESOURCES);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> schedRegistered;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureSatisfy(&schedRegistered));

  Future<vector<Offer>> offers1, offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(schedRegistered);

  // Launch a task requesting no GPUs and
  // verify that running `nvidia-smi` fails.
  AWAIT_READY(offers1);
  EXPECT_EQ(1u, offers1->size());

  TaskInfo task1 = createTask(
      offers1.get()[0].slave_id(),
      Resources::parse("cpus:0.1;mem:128").get(),
      "nvidia-smi");

  Future<TaskStatus> statusStarting1, statusRunning1, statusFailed1;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting1))
    .WillOnce(FutureArg<1>(&statusRunning1))
    .WillOnce(FutureArg<1>(&statusFailed1));

  driver.launchTasks(offers1.get()[0].id(), {task1});

  AWAIT_READY(statusStarting1);
  ASSERT_EQ(TASK_STARTING, statusStarting1->state());

  AWAIT_READY(statusRunning1);
  ASSERT_EQ(TASK_RUNNING, statusRunning1->state());

  AWAIT_READY(statusFailed1);
  ASSERT_EQ(TASK_FAILED, statusFailed1->state());

  // Launch a task requesting 1 GPU and verify
  // that `nvidia-smi` lists exactly one GPU.
  AWAIT_READY(offers2);
  EXPECT_EQ(1u, offers2->size());

  TaskInfo task2 = createTask(
      offers1.get()[0].slave_id(),
      Resources::parse("cpus:0.1;mem:128;gpus:1").get(),
      "NUM_GPUS=`nvidia-smi --list-gpus | wc -l`;\n"
      "if [ \"$NUM_GPUS\" != \"1\" ]; then\n"
      "  exit 1;\n"
      "fi");

  Future<TaskStatus> statusStarting2, statusRunning2, statusFinished2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting2))
    .WillOnce(FutureArg<1>(&statusRunning2))
    .WillOnce(FutureArg<1>(&statusFinished2));

  driver.launchTasks(offers2.get()[0].id(), {task2});

  AWAIT_READY(statusStarting2);
  ASSERT_EQ(TASK_STARTING, statusStarting2->state());

  AWAIT_READY(statusRunning2);
  ASSERT_EQ(TASK_RUNNING, statusRunning2->state());

  AWAIT_READY(statusFinished2);
  ASSERT_EQ(TASK_FINISHED, statusFinished2->state());

  driver.stop();
  driver.join();
}


// This test verifies that we can enable the Nvidia GPU isolator
// and launch tasks with restricted access to GPUs while running
// inside one of Nvidia's images. These images have a special
// label that indicates that we need to mount a volume containing
// the Nvidia libraries and binaries. We first launch a task with
// 1 GPU and verify that a call to `nvidia-smi` both succeeds and
// reports exactly 1 GPU available. We then launch a task with
// access to 0 GPUs and verify that a call to `nvidia-smi` fails.
TEST_F(NvidiaGpuTest, ROOT_INTERNET_CURL_CGROUPS_NVIDIA_GPU_NvidiaDockerImage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux,"
                    "cgroups/devices,gpu/nvidia";
  flags.image_providers = "docker";
  flags.nvidia_gpu_devices = vector<unsigned int>({0u});
  flags.resources = "cpus:1;mem:128;gpus:1";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // NOTE: We use the default executor (and thus v1 API) in this test to avoid
  // executor registration timing out due to fetching the 'nvidia/cuda' image
  // over a slow connection.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      v1::FrameworkInfo::Capability::GPU_RESOURCES);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  EXPECT_CALL(*scheduler, failure(_, _))
    .Times(AtMost(2));

  v1::scheduler::TestMesos mesos(
    master.get()->pid,
    ContentType::PROTOBUF,
    scheduler);

  AWAIT_READY(subscribed);

  const v1::FrameworkID& frameworkId = subscribed->framework_id();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::AgentID& agentId = offers->offers(0).agent_id();

  mesos::v1::Image image;
  image.set_type(mesos::v1::Image::DOCKER);
  image.mutable_docker()->set_name("nvidia/cuda");

  // Launch a task requesting 1 GPU and verify that `nvidia-smi` lists exactly
  // one GPU.
  v1::ExecutorInfo executor1 = v1::createExecutorInfo(
      id::UUID::random().toString(),
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  v1::TaskInfo task1 = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;gpus:1").get(),
      "NUM_GPUS=`nvidia-smi --list-gpus | wc -l`;\n"
      "if [ \"$NUM_GPUS\" != \"1\" ]; then\n"
      "  exit 1;\n"
      "fi");

  mesos::v1::ContainerInfo* container1 = task1.mutable_container();
  container1->set_type(mesos::v1::ContainerInfo::MESOS);
  container1->mutable_mesos()->mutable_image()->CopyFrom(image);

  // Launch a task requesting no GPU and verify that running `nvidia-smi` fails.
  v1::ExecutorInfo executor2 = v1::createExecutorInfo(
      id::UUID::random().toString(),
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  v1::TaskInfo task2 = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32").get(),
      "nvidia-smi");

  mesos::v1::ContainerInfo* container2 = task2.mutable_container();
  container2->set_type(mesos::v1::ContainerInfo::MESOS);
  container2->mutable_mesos()->mutable_image()->CopyFrom(image);

  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_STARTING)))
    .Times(2)
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_RUNNING)))
    .Times(2)
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  Future<v1::scheduler::Event::Update> task1Finished;
  EXPECT_CALL(*scheduler, update(_, AllOf(
      TaskStatusUpdateTaskIdEq(task1.task_id()),
      Truly([](const v1::scheduler::Event::Update& update) {
        return protobuf::isTerminalState(devolve(update.status()).state());
      }))))
    .WillOnce(DoAll(
        FutureArg<1>(&task1Finished),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Update> task2Failed;
  EXPECT_CALL(*scheduler, update(_, AllOf(
      TaskStatusUpdateTaskIdEq(task2.task_id()),
      Truly([](const v1::scheduler::Event::Update& update) {
        return protobuf::isTerminalState(devolve(update.status()).state());
      }))))
    .WillOnce(DoAll(
        FutureArg<1>(&task2Failed),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offers->offers(0),
      {v1::LAUNCH_GROUP(executor1, v1::createTaskGroupInfo({task1})),
       v1::LAUNCH_GROUP(executor2, v1::createTaskGroupInfo({task2}))}));

  // We wait up to 180 seconds to download the docker image.
  AWAIT_READY_FOR(task1Finished, Seconds(180));
  EXPECT_EQ(v1::TASK_FINISHED, task1Finished->status().state());

  AWAIT_READY(task2Failed);
  EXPECT_EQ(v1::TASK_FAILED, task2Failed->status().state());
}


// This test verifies that we can enable the Nvidia GPU isolator and launch
// tasks to run the Tensorflow GPU image, which is based on Nvidia's image that
// has a special environment variable that indicates that we need to mount a
// volume containing the Nvidia libraries and binaries.
TEST_F(NvidiaGpuTest, ROOT_INTERNET_CURL_CGROUPS_NVIDIA_GPU_TensorflowGpuImage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux,"
                    "cgroups/devices,gpu/nvidia";
  flags.image_providers = "docker";
  flags.nvidia_gpu_devices = vector<unsigned int>({0u});
  flags.resources = "cpus:1;mem:128;gpus:1";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // NOTE: We use the default executor (and thus v1 API) in this test to avoid
  // executor registration timing out due to fetching the Tensorflow GPU image
  // over a slow connection.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      v1::FrameworkInfo::Capability::GPU_RESOURCES);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  EXPECT_CALL(*scheduler, failure(_, _))
    .Times(AtMost(2));

  v1::scheduler::TestMesos mesos(
    master.get()->pid,
    ContentType::PROTOBUF,
    scheduler);

  AWAIT_READY(subscribed);

  const v1::FrameworkID& frameworkId = subscribed->framework_id();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::AgentID& agentId = offers->offers(0).agent_id();

  mesos::v1::Image image;
  image.set_type(mesos::v1::Image::DOCKER);
  image.mutable_docker()->set_name("tensorflow/tensorflow:latest-gpu");

  // Launch a task requesting 1 GPU and run a simple Tensorflow program.
  v1::ExecutorInfo executor = v1::createExecutorInfo(
      id::UUID::random().toString(),
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  v1::TaskInfo task = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;gpus:1").get(),
      "python -c '"
      "import tensorflow as tf;"
      "tf.enable_eager_execution();"
      "print(tf.reduce_sum(tf.random_normal([1000, 1000])));"
      "'");

  mesos::v1::ContainerInfo* container = task.mutable_container();
  container->set_type(mesos::v1::ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_STARTING)))
    .WillOnce(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_RUNNING)))
    .WillOnce(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  Future<v1::scheduler::Event::Update> terminalStatusUpdate;
  EXPECT_CALL(*scheduler, update(
      _,
      Truly([](const v1::scheduler::Event::Update& update) {
        return protobuf::isTerminalState(devolve(update.status()).state());
      })))
    .WillOnce(DoAll(
        FutureArg<1>(&terminalStatusUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offers->offers(0),
      {v1::LAUNCH_GROUP(executor, v1::createTaskGroupInfo({task}))}));

  // We wait up to 180 seconds to download the docker image.
  AWAIT_READY_FOR(terminalStatusUpdate, Seconds(180));
  EXPECT_EQ(v1::TASK_FINISHED, terminalStatusUpdate->status().state());
}


// This test verifies correct failure semantics when
// a task requests a fractional number of GPUs.
TEST_F(NvidiaGpuTest, ROOT_CGROUPS_NVIDIA_GPU_FractionalResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Turn on Nvidia GPU isolation.
  // Assume at least one GPU is available for isolation.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,cgroups/devices,gpu/nvidia";
  flags.nvidia_gpu_devices = vector<unsigned int>({0u});
  flags.resources = "gpus:1";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::GPU_RESOURCES);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> schedRegistered;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureSatisfy(&schedRegistered));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(schedRegistered);

  // Launch a task requesting a fractional number
  // of GPUs and verify that it fails as expected.
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      Resources::parse("cpus:0.1;mem:128;gpus:0.1").get(),
      "true");

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);

  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status->reason());
  EXPECT_TRUE(strings::contains(
      status->message(),
      "The 'gpus' resource must be an unsigned integer"));

  driver.stop();
  driver.join();
}


// Ensures that GPUs can be auto-discovered.
TEST_F(NvidiaGpuTest, NVIDIA_GPU_Discovery)
{
  ASSERT_TRUE(nvml::isAvailable());
  ASSERT_SOME(nvml::initialize());

  Try<unsigned int> gpus = nvml::deviceGetCount();
  ASSERT_SOME(gpus);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:1"; // To override the default with gpus:0.
  flags.isolation = "gpu/nvidia";

  Try<Resources> resources = Containerizer::resources(flags);

  ASSERT_SOME(resources);
  ASSERT_SOME(resources->gpus());
  ASSERT_EQ(gpus.get(), resources->gpus().get());
}


// Ensures that the --resources and --nvidia_gpu_devices
// flags are correctly validated.
TEST_F(NvidiaGpuTest, ROOT_CGROUPS_NVIDIA_GPU_FlagValidation)
{
  ASSERT_TRUE(nvml::isAvailable());
  ASSERT_SOME(nvml::initialize());

  Try<unsigned int> gpus = nvml::deviceGetCount();
  ASSERT_SOME(gpus);

  // Not setting the `gpu/nvidia` isolation flag
  // should not trigger-autodiscovery!
  slave::Flags flags = CreateSlaveFlags();

  Try<Resources> resources = NvidiaGpuAllocator::resources(flags);

  ASSERT_SOME(resources);
  ASSERT_NONE(resources->gpus());

  // Setting `--nvidia_gpu_devices` without the `gpu/nvidia`
  // isolation flag should trigger an error.
  flags = CreateSlaveFlags();
  flags.nvidia_gpu_devices = vector<unsigned int>({0u});
  flags.resources = "gpus:1";

  resources = Containerizer::resources(flags);

  ASSERT_ERROR(resources);

  // Setting GPUs without the `gpu/nvidia` isolation
  // flag should just pass them through without an error.
  flags = CreateSlaveFlags();
  flags.resources = "gpus:100";

  resources = Containerizer::resources(flags);

  ASSERT_SOME(resources);
  ASSERT_SOME(resources->gpus());
  ASSERT_EQ(100u, resources->gpus().get());

  // Setting the `gpu/nvidia` isolation
  // flag should trigger autodiscovery.
  flags = CreateSlaveFlags();
  flags.resources = "cpus:1"; // To override the default with gpus:0.
  flags.isolation = "gpu/nvidia";

  resources = NvidiaGpuAllocator::resources(flags);

  ASSERT_SOME(resources);
  ASSERT_SOME(resources->gpus());
  ASSERT_EQ(gpus.get(), resources->gpus().get());

  // Setting the GPUs to 0 should not trigger auto-discovery!
  flags = CreateSlaveFlags();
  flags.resources = "gpus:0";
  flags.isolation = "gpu/nvidia";

  resources = Containerizer::resources(flags);

  ASSERT_SOME(resources);
  ASSERT_NONE(resources->gpus());

  // --nvidia_gpu_devices and --resources agree on the number of GPUs.
  flags = CreateSlaveFlags();
  flags.nvidia_gpu_devices = vector<unsigned int>({0u});
  flags.resources = "gpus:1";
  flags.isolation = "gpu/nvidia";

  resources = NvidiaGpuAllocator::resources(flags);

  ASSERT_SOME(resources);
  ASSERT_SOME(resources->gpus());
  ASSERT_EQ(1u, resources->gpus().get());

  // Both --resources and --nvidia_gpu_devices must be specified!
  flags = CreateSlaveFlags();
  flags.nvidia_gpu_devices = vector<unsigned int>({0u});
  flags.resources = "cpus:1"; // To override the default with gpus:0.
  flags.isolation = "gpu/nvidia";

  resources = NvidiaGpuAllocator::resources(flags);

  ASSERT_ERROR(resources);

  flags = CreateSlaveFlags();
  flags.resources = "gpus:" + stringify(gpus.get());
  flags.isolation = "gpu/nvidia";

  resources = NvidiaGpuAllocator::resources(flags);

  ASSERT_ERROR(resources);

  // --nvidia_gpu_devices and --resources do not match!
  flags = CreateSlaveFlags();
  flags.nvidia_gpu_devices = vector<unsigned int>({0u});
  flags.resources = "gpus:2";
  flags.isolation = "gpu/nvidia";

  resources = NvidiaGpuAllocator::resources(flags);

  ASSERT_ERROR(resources);

  flags = CreateSlaveFlags();
  flags.nvidia_gpu_devices = vector<unsigned int>({0u});
  flags.resources = "gpus:0";
  flags.isolation = "gpu/nvidia";

  resources = NvidiaGpuAllocator::resources(flags);

  ASSERT_ERROR(resources);

  // More than available on the machine!
  flags = CreateSlaveFlags();
  flags.nvidia_gpu_devices = vector<unsigned int>();
  flags.resources = "gpus:" + stringify(2 * gpus.get());
  flags.isolation = "gpu/nvidia";

  for (size_t i = 0; i < 2 * gpus.get(); ++i) {
    flags.nvidia_gpu_devices->push_back(i);
  }

  resources = NvidiaGpuAllocator::resources(flags);

  ASSERT_ERROR(resources);

  // Set `nvidia_gpu_devices` to contain duplicates.
  flags = CreateSlaveFlags();
  flags.nvidia_gpu_devices = vector<unsigned int>({0u, 0u});
  flags.resources = "cpus:1;gpus:1";
  flags.isolation = "gpu/nvidia";

  resources = NvidiaGpuAllocator::resources(flags);

  ASSERT_ERROR(resources);
}


// Test proper allocation / deallocation of GPU devices.
TEST_F(NvidiaGpuTest, NVIDIA_GPU_Allocator)
{
  ASSERT_TRUE(nvml::isAvailable());
  ASSERT_SOME(nvml::initialize());

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:1"; // To override the default with gpus:0.
  flags.isolation = "gpu/nvidia";

  Try<Resources> resources = NvidiaGpuAllocator::resources(flags);
  ASSERT_SOME(resources);

  Try<NvidiaGpuAllocator> allocator =
    NvidiaGpuAllocator::create(flags, resources.get());
  ASSERT_SOME(allocator);

  Try<unsigned int> total = nvml::deviceGetCount();
  ASSERT_SOME(total);
  ASSERT_GE(total.get(), 1u);
  ASSERT_EQ(total.get(), allocator->total().size());

  // Allocate all GPUs at once.
  Future<set<Gpu>> gpus = allocator->allocate(total.get());

  AWAIT_READY(gpus);
  ASSERT_EQ(total.get(), gpus->size());

  // Make sure there are no GPUs left to allocate.
  AWAIT_FAILED(allocator->allocate(1));

  // Free all GPUs at once and reallocate them by reference.
  AWAIT_READY(allocator->deallocate(gpus.get()));
  AWAIT_READY(allocator->allocate(gpus.get()));

  // Free 1 GPU back and reallocate it. Make sure they are the same.
  AWAIT_READY(allocator->deallocate({ *gpus->begin() }));

  Future<set<Gpu>> gpu = allocator->allocate(1);
  AWAIT_READY(gpu);
  ASSERT_EQ(*gpus->begin(), *gpu->begin());

  // Attempt to free the same GPU twice.
  AWAIT_READY(allocator->deallocate({ *gpus->begin() }));
  AWAIT_FAILED(allocator->deallocate({ *gpus->begin() }));

  // Allocate a specific GPU by reference.
  AWAIT_READY(allocator->allocate({ *gpus->begin() }));

  // Attempt to free a bogus GPU.
  Gpu bogus;
  bogus.major = 999;
  bogus.minor = 999;

  AWAIT_FAILED(allocator->deallocate({ bogus }));

  // Free all GPUs.
  AWAIT_READY(allocator->deallocate(gpus.get()));

  // Attempt to allocate a bogus GPU.
  AWAIT_FAILED(allocator->allocate({ bogus }));
}


// Tests that we can create the volume that consolidates
// the Nvidia libraries and binaries.
TEST_F(NvidiaGpuTest, ROOT_NVIDIA_GPU_VolumeCreation)
{
  Try<NvidiaVolume> volume = NvidiaVolume::create();
  ASSERT_SOME(volume);

  ASSERT_TRUE(os::exists(volume->HOST_PATH()));

  vector<string> directories = { "bin", "lib", "lib64" };
  foreach (const string& directory, directories) {
    EXPECT_TRUE(os::exists(volume->HOST_PATH() + "/" + directory));
  }

  EXPECT_TRUE(os::exists(volume->HOST_PATH() + "/bin/nvidia-smi"));
  EXPECT_TRUE(os::exists(volume->HOST_PATH() + "/lib64/libnvidia-ml.so.1"));
}


// Tests that we can properly detect when an Nvidia volume should be
// injected into a Docker container given its ImageManifest.
TEST_F(NvidiaGpuTest, ROOT_NVIDIA_GPU_VolumeShouldInject)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(
      R"~(
      {
        "config": {
          "Labels": {
            "com.nvidia.volumes.needed": "nvidia_driver"
          }
        }
      })~");

  ASSERT_SOME(json);

  Try<::docker::spec::v1::ImageManifest> manifest =
    ::docker::spec::v1::parse(json.get());
  ASSERT_SOME(manifest);

  Try<NvidiaVolume> volume = NvidiaVolume::create();
  ASSERT_SOME(volume);

  ASSERT_TRUE(volume->shouldInject(manifest.get()));

  json = JSON::parse<JSON::Object>(
      R"~(
      {
        "config": {
          "Labels": {
            "com.ati.volumes.needed": "ati_driver"
          }
        }
      })~");

  ASSERT_SOME(json);

  manifest = ::docker::spec::v1::parse(json.get());
  ASSERT_SOME(manifest);

  volume = NvidiaVolume::create();
  ASSERT_SOME(volume);

  ASSERT_FALSE(volume->shouldInject(manifest.get()));
}


// This test verifies that the DefaultExecutor is able to launch tasks
// with restricted access to GPUs.
// It launches a task with 1 GPU and verifies that a call to
// `nvidia-smi` both succeeds and reports exactly 1 GPU available.
TEST_F(NvidiaGpuTest, ROOT_CGROUPS_NVIDIA_GPU_DefaultExecutorVerifyDeviceAccess)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Turn on Nvidia GPU isolation.
  // Assume at least one GPU is available for isolation.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,cgroups/devices,gpu/nvidia";
  flags.resources = "cpus:1"; // To override the default with gpus:0.

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::GPU_RESOURCES);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  Resources resources = Resources::parse("cpus:0.1;mem:32;disk:32").get();

  ExecutorInfo executorInfo;
  executorInfo.set_type(ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId.get());
  executorInfo.mutable_resources()->CopyFrom(resources);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers->front();
  const SlaveID& slaveId = offer.slave_id();

  TaskInfo taskInfo = createTask(
      slaveId,
      Resources::parse("cpus:0.1;mem:128;gpus:1").get(),
      "NUM_GPUS=`nvidia-smi --list-gpus | wc -l`;\n"
      "if [ \"$NUM_GPUS\" != \"1\" ]; then\n"
      "  exit 1;\n"
      "fi");

  TaskGroupInfo taskGroup = createTaskGroupInfo({taskInfo});

  Future<TaskStatus> statusStarting, statusRunning, statusFinished;

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.acceptOffers({offer.id()}, {LAUNCH_GROUP(executorInfo, taskGroup)});

  AWAIT_READY(statusStarting);
  ASSERT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  ASSERT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  ASSERT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
