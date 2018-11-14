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

#include <unistd.h>

#include <list>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/check.hpp>
#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/latch.hpp>
#include <process/message.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/queue.hpp>
#include <process/subprocess.hpp>

#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/gc.hpp"
#include "slave/flags.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "tests/containerizer.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_fetcher.hpp"
#include "tests/utils.hpp"

using mesos::fetcher::FetcherInfo;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;
using mesos::internal::slave::Containerizer;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::FetcherProcess;

using mesos::master::detector::MasterDetector;

using process::TEST_AWAIT_TIMEOUT;
using process::Future;
using process::HttpEvent;
using process::Latch;
using process::Owned;
using process::PID;
using process::Process;
using process::Promise;
using process::Queue;
using process::Subprocess;

using std::cout;
using std::endl;
using std::list;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::Invoke;
using testing::InvokeWithoutArgs;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

static const string ASSETS_DIRECTORY_NAME = "mesos-fetcher-test-assets";
static const string COMMAND_NAME = "mesos-fetcher-test-cmd";
static const string ARCHIVE_NAME = "mesos-fetcher-test-archive.tgz";
static const string ARCHIVED_COMMAND_NAME = "mesos-fetcher-test-acmd";

// Every task executes one of these shell scripts, which create a
// file that includes the current task name in its name. The latter
// is expected to be passed in as a script argument. The existence
// of the file with that name is then used as proof that the task
// ran successfully.
static const string COMMAND_SCRIPT = "touch " + COMMAND_NAME + "$1";
static const string ARCHIVED_COMMAND_SCRIPT =
  "touch " + ARCHIVED_COMMAND_NAME + "$1";


class FetcherCacheTest : public MesosTest
{
public:
  // A helper struct that captures useful information for each of the
  // tasks that we have launched to help test expectations.
  struct Task
  {
    Path runDirectory;
    Queue<TaskStatus> statusQueue;
  };

  void setupCommandFileAsset();

protected:
  void setupArchiveAsset();

  void SetUp() override;
  void TearDown() override;

  // Sets up the slave and starts it. Calling this late in the test
  // instead of having it included in SetUp() gives us the opportunity
  // to manipulate values in 'flags', first.
  void startSlave();

  // Stops the slave, deleting the containerizer, for subsequent
  // recovery testing.
  void stopSlave();

  Try<Task> launchTask(const CommandInfo& commandInfo, size_t taskIndex);

  Try<vector<Task>> launchTasks(const vector<CommandInfo>& commandInfos);

  void verifyCacheMetrics();

  // Promises whose futures indicate that FetcherProcess::_fetch() has been
  // called for a task with a given index.
  vector<Owned<Promise<Nothing>>> fetchContentionWaypoints;

  string assetsDirectory;
  string commandPath;
  string archivePath;

  Owned<cluster::Master> master;
  Owned<cluster::Slave> slave;

  slave::Flags flags;
  SlaveID slaveId;

  Owned<MasterDetector> detector;
  Owned<MesosContainerizer> containerizer;

  // NOTE: This is technically owned by the `fetcher`, but we violate
  // this ownership in the tests.
  MockFetcherProcess* fetcherProcess;

  MockScheduler scheduler;
  Owned<MesosSchedulerDriver> driver;

private:
  Owned<Fetcher> fetcher;

  FrameworkID frameworkId;

  // If this test did not succeed as indicated by the above variable,
  // the contents of these sandboxes will be dumped during tear down.
  vector<Path> sandboxes;
};


void FetcherCacheTest::SetUp()
{
  MesosTest::SetUp();

  flags = CreateSlaveFlags();
  flags.resources = "cpus:1000;mem:1000";

  assetsDirectory = path::join(flags.work_dir, ASSETS_DIRECTORY_NAME);
  ASSERT_SOME(os::mkdir(assetsDirectory));

  setupCommandFileAsset();
  setupArchiveAsset();

  Try<Owned<cluster::Master>> _master = StartMaster();
  ASSERT_SOME(_master);
  master = _master.get();

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_name("default");
  frameworkInfo.set_checkpoint(true);

  driver.reset(new MesosSchedulerDriver(
    &scheduler, frameworkInfo, master->pid, DEFAULT_CREDENTIAL));

  EXPECT_CALL(scheduler, registered(driver.get(), _, _));

  // This installs a temporary reaction to resourceOffers calls, which
  // must be in place BEFORE starting the scheduler driver. This
  // "cover" is necessary, because we only add relevant mock actions
  // in launchTask() and launchTasks() AFTER starting the driver.
  EXPECT_CALL(scheduler, resourceOffers(driver.get(), _))
    .WillRepeatedly(DeclineOffers());
}


// Dumps the contents of a text file to cout, assuming
// there are only text files.
static void logFile(const Path& path, const string& filename)
{
  string filePath = path::join(path.string(), filename);
  Try<string> text = os::read(filePath);
  if (text.isSome()) {
    cout << "Begin file contents of `" << filename << "`:" << endl;
    cout << text.get() << endl;
    cout << "End file" << endl;
  } else {
    cout << "File `" << filename << "` not readable: " << text.error() << endl;
  }
}


// Dumps the contents of all files in the sandbox to cout, assuming
// there are only text files.
static void logSandbox(const Path& path)
{
  Try<list<string>> entries = os::ls(path.string());
  if (entries.isSome()) {
    cout << "Begin listing sandbox `" << path.string() << "`:" << endl;
    foreach (const string& entry, entries.get()) {
      logFile(path, entry);
    }
    cout << "End sandbox" << endl;
  } else {
    cout << "Could not list sandbox `" << path.string()
         << "`: " << entries.error() << endl;
  }
}


void FetcherCacheTest::verifyCacheMetrics()
{
  JSON::Object metrics = Metrics();

  ASSERT_EQ(
      1u,
      metrics.values.count("containerizer/fetcher/cache_size_total_bytes"));

  // The total size is always given by the corresponding agent flag.
  EXPECT_SOME_EQ(
      flags.fetcher_cache_size.bytes(),
      metrics.at<JSON::Number>("containerizer/fetcher/cache_size_total_bytes"));

  Try<std::list<Path>> files = fetcherProcess->cacheFiles();
  ASSERT_SOME(files);

  Bytes used;

  foreach (const auto& file, files.get()) {
    Try<Bytes> size = os::stat::size(file);
    ASSERT_SOME(size);

    used += size.get();
  }

  ASSERT_EQ(
      1u,
      metrics.values.count("containerizer/fetcher/cache_size_used_bytes"));

  // Verify that the used amount of cache is the total of the size of
  // all the files in the cache.
  EXPECT_SOME_EQ(
      used.bytes(),
      metrics.at<JSON::Number>("containerizer/fetcher/cache_size_used_bytes"));
}


void FetcherCacheTest::TearDown()
{
  if (HasFatalFailure()) {
    // A gtest macro has terminated the test prematurely. Now stream
    // additional info that might help debug the situation to where
    // gtest writes its output: cout.

    cout << "Begin listing sandboxes" << endl;
    foreach (const Path& path, sandboxes) {
      logSandbox(path);
    }
    cout << "End sandboxes" << endl;
  }

  driver->stop();
  driver->join();

  master.reset();
  slave.reset();

  MesosTest::TearDown();
}


// TODO(bernd-mesos): Make this abstractions as generic and generally
// available for all testing as possible.
void FetcherCacheTest::startSlave()
{
  fetcherProcess = new MockFetcherProcess(flags);
  fetcher.reset(new Fetcher(Owned<FetcherProcess>(fetcherProcess)));

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags, true, fetcher.get());

  ASSERT_SOME(create);
  containerizer.reset(create.get());

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  detector = master->createDetector();

  Try<Owned<cluster::Slave>> _slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(_slave);
  slave = _slave.get();

  AWAIT_READY(slaveRegisteredMessage);
  slaveId = slaveRegisteredMessage->slave_id();
}


void FetcherCacheTest::setupCommandFileAsset()
{
  commandPath = path::join(assetsDirectory, COMMAND_NAME);
  ASSERT_SOME(os::write(commandPath, COMMAND_SCRIPT));

  // Make the command file read-only, so we can discern the URI
  // executable flag.
  ASSERT_SOME(os::chmod(commandPath, S_IRUSR | S_IRGRP | S_IROTH));
}


void FetcherCacheTest::setupArchiveAsset()
{
  string path = path::join(assetsDirectory, ARCHIVED_COMMAND_NAME);
  ASSERT_SOME(os::write(path, ARCHIVED_COMMAND_SCRIPT));

  // Make the archived command file executable before archiving it,
  // since the executable flag for CommandInfo::URI has no effect on
  // what comes out of an archive.
  ASSERT_SOME(os::chmod(path, S_IRWXU | S_IRWXG | S_IRWXO));

  const string cwd = os::getcwd();
  ASSERT_SOME(os::chdir(assetsDirectory));
  // Create an uncompressed archive (see MESOS-3579).
  ASSERT_SOME(os::shell(
      "tar cf '" + ARCHIVE_NAME + "' '" + ARCHIVED_COMMAND_NAME + "' 2>&1"));
  ASSERT_SOME(os::chdir(cwd));
  archivePath = path::join(assetsDirectory, ARCHIVE_NAME);

  // Make the archive file read-only, so we can tell if it becomes
  // executable by accident.
  ASSERT_SOME(os::chmod(archivePath, S_IRUSR | S_IRGRP | S_IROTH));
}


static string taskName(int taskIndex)
{
  return stringify(taskIndex);
}


// TODO(bernd-mesos): Use Path, not string, create Path::executable().
static bool isExecutable(const string& path)
{
  Try<bool> access = os::access(path, X_OK);
  EXPECT_SOME(access);
  return access.isSome() && access.get();
}


// Create a future that indicates that the task observed by the given
// status queue is finished.
static Future<Nothing> awaitFinished(FetcherCacheTest::Task task)
{
  return task.statusQueue.get()
    .then([=](const TaskStatus& status) -> Future<Nothing> {
      if (status.state() == TASK_FINISHED) {
        return Nothing();
      }
      return awaitFinished(task);
  });
}


// Create a future that indicates that all tasks are finished.
// TODO(bernd-mesos): Make this abstractions as generic and generally
// available for all testing as possible.
static Future<vector<Nothing>> awaitFinished(
    vector<FetcherCacheTest::Task> tasks)
{
  vector<Future<Nothing>> futures;

  foreach (FetcherCacheTest::Task task, tasks) {
    futures.push_back(awaitFinished(task));
  }

  return collect(futures);
}


// Pushes the TaskStatus value in mock call argument #1 into the
// given queue, which later on shall be queried by awaitFinished().
ACTION_P(PushTaskStatus, taskStatusQueue)
{
  const TaskStatus& taskStatus = arg1;

  // Input parameters of ACTION_P are const. We make a mutable copy
  // so that we can use put().
  Queue<TaskStatus> queue = taskStatusQueue;

  queue.put(taskStatus);
}


// Launches a task as described by its CommandInfo and returns its sandbox
// run directory path. Its completion will be indicated by the result of
// awaitFinished(task), where `task` is the return value of this method.
// TODO(bernd-mesos): Make this abstraction as generic and generally
// available for all testing as possible.
Try<FetcherCacheTest::Task> FetcherCacheTest::launchTask(
    const CommandInfo& commandInfo,
    size_t taskIndex)
{
  Future<vector<Offer>> offers;
  EXPECT_CALL(scheduler, resourceOffers(driver.get(), _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers());

  offers.await(TEST_AWAIT_TIMEOUT);
  if (!offers.isReady()) {
    return Error("Failed to wait for resource offers: " +
           (offers.isFailed() ? offers.failure() : "discarded"));
  }

  if (offers->empty()) {
    return Error("Received empty list of offers");
  }
  const Offer offer = offers.get()[0];

  TaskInfo task;
  task.set_name(taskName(taskIndex));
  task.mutable_task_id()->set_value(taskName(taskIndex));
  task.mutable_slave_id()->CopyFrom(offer.slave_id());

  // We don't care about resources in these tests. This small amount
  // will always succeed.
  task.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1;mem:1").get());

  task.mutable_command()->CopyFrom(commandInfo);

  // Since we are always using a command executor here, the executor
  // ID can be determined by copying the task ID.
  ExecutorID executorId;
  executorId.set_value(task.task_id().value());

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Queue<TaskStatus> taskStatusQueue;

  EXPECT_CALL(scheduler, statusUpdate(driver.get(), _))
    .WillRepeatedly(PushTaskStatus(taskStatusQueue));

  driver->launchTasks(offer.id(), tasks);

  const Path sandboxPath = Path(slave::paths::getExecutorLatestRunPath(
      flags.work_dir,
      slaveId,
      offer.framework_id(),
      executorId));

  sandboxes.push_back(sandboxPath);

  return Task{sandboxPath, taskStatusQueue};
}


// Pushes the task status value of a task status update callback
// into the task status queue that corresponds to the task index/ID
// for which the status update is being reported. 'tasks' must be a
// 'vector<Task>>', where every slot index corresponds to a task
// index/ID.
// TODO(bernd-mesos): Make this abstractions as generic and generally
// available for all testing as possible.
ACTION_TEMPLATE(PushIndexedTaskStatus,
                HAS_1_TEMPLATE_PARAMS(int, k),
                AND_1_VALUE_PARAMS(tasks))
{
  const TaskStatus& taskStatus = ::std::get<k>(args);
  Try<int> taskId = numify<int>(taskStatus.task_id().value());
  ASSERT_SOME(taskId);
  Queue<TaskStatus> queue = (tasks)[taskId.get()].statusQueue;
  queue.put(taskStatus);
}


// Satisfies the first promise in the list that is not satisfied yet.
ACTION_P(SatisfyOne, promises)
{
  foreach (const Owned<Promise<Nothing>>& promise, *promises) {
    if (promise->future().isPending()) {
      promise->set(Nothing());
      return;
    }
  }

  FAIL() << "Tried to call FetcherProcess::_fetch() "
         << "for more tasks than launched";
}


// Launches the tasks described by the given CommandInfo and returns a
// vector holding the run directory paths. All these tasks run
// concurrently. Their completion will be indicated by the result of
// awaitFinished(tasks), where `tasks` is the return value of this
// method.
// TODO(bernd-mesos): Make this abstraction as generic and generally
// available for all testing as possible.
Try<vector<FetcherCacheTest::Task>> FetcherCacheTest::launchTasks(
    const vector<CommandInfo>& commandInfos)
{
  vector<FetcherCacheTest::Task> result;

  // When _fetch() is called, notify us by satisfying a promise that
  // a task has passed the code stretch in which it competes for cache
  // entries.
  EXPECT_CALL(*fetcherProcess, _fetch(_, _, _, _, _))
    .WillRepeatedly(
        DoAll(SatisfyOne(&fetchContentionWaypoints),
              Invoke(fetcherProcess, &MockFetcherProcess::unmocked__fetch)));

  Future<vector<Offer>> offers;
  EXPECT_CALL(scheduler, resourceOffers(driver.get(), _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers());

  offers.await(TEST_AWAIT_TIMEOUT);
  if (!offers.isReady()) {
    return Error("Failed to wait for resource offers: " +
           (offers.isFailed() ? offers.failure() : "discarded"));
  }

  EXPECT_FALSE(offers->empty());
  const Offer offer = offers.get()[0];

  vector<TaskInfo> tasks;
  foreach (const CommandInfo& commandInfo, commandInfos) {
    size_t taskIndex = tasks.size();

    // Grabbing the framework ID from somewhere. It should not matter
    // if this happens several times, as we expect the framework ID to
    // remain the same.
    frameworkId = offer.framework_id();

    TaskInfo task;
    task.set_name(taskName(taskIndex));
    task.mutable_task_id()->set_value(taskName(taskIndex));
    task.mutable_slave_id()->CopyFrom(offer.slave_id());

    // We don't care about resources in these tests. This small amount
    // will always succeed.
    task.mutable_resources()->CopyFrom(
        Resources::parse("cpus:1;mem:1").get());

    task.mutable_command()->CopyFrom(commandInfo);

    tasks.push_back(task);

    // Since we are always using a command executor here, the executor
    // ID can be determined by copying the task ID.
    ExecutorID executorId;
    executorId.set_value(task.task_id().value());

    Path sandboxPath = Path(slave::paths::getExecutorLatestRunPath(
        flags.work_dir,
        slaveId,
        frameworkId,
        executorId));

    sandboxes.push_back(sandboxPath);

    // Grabbing task status futures to wait for. We make a queue of futures
    // for each task. We can then wait until the front element indicates
    // status TASK_FINISHED. We use a queue, because we never know which
    // status update will be the one we have been waiting for.
    Queue<TaskStatus> taskStatusQueue;

    result.push_back(Task {sandboxPath, taskStatusQueue});

    auto waypoint = Owned<Promise<Nothing>>(new Promise<Nothing>());
    fetchContentionWaypoints.push_back(waypoint);
  }

  EXPECT_CALL(scheduler, statusUpdate(driver.get(), _))
    .WillRepeatedly(PushIndexedTaskStatus<1>(result));

  driver->launchTasks(offer.id(), tasks);

  return result;
}


// Tests fetching from the local asset directory without cache. This
// gives us a baseline for the following tests and lets us debug our
// test infrastructure without extra complications.
TEST_F(FetcherCacheTest, LocalUncached)
{
  startSlave();
  driver->start();

  const int index = 0;
  CommandInfo::URI uri;
  uri.set_value(commandPath);
  uri.set_executable(true);

  CommandInfo commandInfo;
  commandInfo.set_value("./" + COMMAND_NAME + " " + taskName(index));
  commandInfo.add_uris()->CopyFrom(uri);

  const Try<Task> task = launchTask(commandInfo, index);
  ASSERT_SOME(task);

  AWAIT_READY(awaitFinished(task.get()));

  EXPECT_EQ(0u, fetcherProcess->cacheSize());
  ASSERT_SOME(fetcherProcess->cacheFiles());
  EXPECT_TRUE(fetcherProcess->cacheFiles()->empty());

  const string path = path::join(task->runDirectory.string(), COMMAND_NAME);
  EXPECT_TRUE(isExecutable(path));
  EXPECT_TRUE(os::exists(path + taskName(index)));
}


// Tests fetching from the local asset directory with simple caching.
// Only one download must occur. Fetching is serialized, to cover
// code areas without overlapping/concurrent fetch attempts.
TEST_F(FetcherCacheTest, LocalCached)
{
  startSlave();
  driver->start();

  for (size_t i = 0; i < 2; i++) {
    CommandInfo::URI uri;
    uri.set_value(commandPath);
    uri.set_executable(true);
    uri.set_cache(true);

    CommandInfo commandInfo;
    commandInfo.set_value("./" + COMMAND_NAME + " " + taskName(i));
    commandInfo.add_uris()->CopyFrom(uri);

    const Try<Task> task = launchTask(commandInfo, i);
    ASSERT_SOME(task);

    AWAIT_READY(awaitFinished(task.get()));

    const string path = path::join(task->runDirectory.string(), COMMAND_NAME);
    EXPECT_TRUE(isExecutable(path));
    EXPECT_TRUE(os::exists(path + taskName(i)));

    EXPECT_EQ(1u, fetcherProcess->cacheSize());
    ASSERT_SOME(fetcherProcess->cacheFiles());
    EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

    verifyCacheMetrics();
  }
}


// This test launches a task with enabled cache, then removes all cached files,
// then attempts to launch another task with the same URIs as the first task.
// We expect that the fetcher retries to download all the artifacts when cached
// files are missing.
TEST_F(FetcherCacheTest, LocalCachedMissing)
{
  startSlave();
  driver->start();

  for (size_t i = 0; i < 2; i++) {
    CommandInfo::URI uri;
    uri.set_value(commandPath);
    uri.set_executable(true);
    uri.set_cache(true);

    CommandInfo commandInfo;
    commandInfo.set_value("./" + COMMAND_NAME + " " + taskName(i));
    commandInfo.add_uris()->CopyFrom(uri);

    const Try<Task> task = launchTask(commandInfo, i);
    ASSERT_SOME(task);

    AWAIT_READY(awaitFinished(task.get()));

    const string path = path::join(task->runDirectory.string(), COMMAND_NAME);
    EXPECT_TRUE(isExecutable(path));
    EXPECT_TRUE(os::exists(path + taskName(i)));

    EXPECT_EQ(1u, fetcherProcess->cacheSize());
    ASSERT_SOME(fetcherProcess->cacheFiles());
    EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

    verifyCacheMetrics();

    EXPECT_SOME(os::rm(fetcherProcess->cacheFiles()->front()));
  }
}


TEST_F(FetcherCacheTest, CachedCustomFilename)
{
  startSlave();
  driver->start();

  const int index = 0;
  const string customOutputFile = "my-command";
  CommandInfo::URI uri;
  uri.set_value(commandPath);
  uri.set_executable(true);
  uri.set_cache(true);
  uri.set_output_file(customOutputFile);

  CommandInfo commandInfo;
  commandInfo.set_value("./" + customOutputFile + " " + taskName(index));
  commandInfo.add_uris()->CopyFrom(uri);

  const Try<Task> task = launchTask(commandInfo, index);
  ASSERT_SOME(task);

  AWAIT_READY(awaitFinished(task.get()));

  EXPECT_EQ(1u, fetcherProcess->cacheSize());
  ASSERT_SOME(fetcherProcess->cacheFiles());
  EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

  verifyCacheMetrics();

  // Verify that the downloaded executable lives at our custom output path.
  const string executablePath = path::join(
    task->runDirectory.string(), customOutputFile);

  EXPECT_TRUE(isExecutable(executablePath));

  // The script specified by COMMAND_SCRIPT just statically touches a file
  // named $COMMAND_NAME + $1, so if we want to verify that it ran here we have
  // to check this path in addition to the custom-named executable we saved.
  const string outputPath = path::join(
    task->runDirectory.string(), COMMAND_NAME);

  EXPECT_TRUE(os::exists(outputPath + taskName(index)));
}


TEST_F(FetcherCacheTest, CachedCustomOutputFileWithSubdirectory)
{
  startSlave();
  driver->start();

  const int index = 0;
  const string customOutputFile = "subdir/my-command";
  CommandInfo::URI uri;
  uri.set_value(commandPath);
  uri.set_executable(true);
  uri.set_cache(true);
  uri.set_output_file(customOutputFile);

  CommandInfo commandInfo;
  commandInfo.set_value("./" + customOutputFile + " " + taskName(index));
  commandInfo.add_uris()->CopyFrom(uri);

  const Try<Task> task = launchTask(commandInfo, index);
  ASSERT_SOME(task);

  AWAIT_READY(awaitFinished(task.get()));

  EXPECT_EQ(1u, fetcherProcess->cacheSize());
  ASSERT_SOME(fetcherProcess->cacheFiles());
  EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

  verifyCacheMetrics();

  // Verify that the downloaded executable lives at our custom output file
  // path.
  const string executablePath = path::join(
      task->runDirectory.string(), customOutputFile);

  EXPECT_TRUE(isExecutable(executablePath));

  // The script specified by COMMAND_SCRIPT just statically touches a file
  // named $COMMAND_NAME + $1, so if we want to verify that it ran here we have
  // to check this path in addition to the custom-named executable we saved.
  const string outputPath = path::join(
      task->runDirectory.string(), COMMAND_NAME);

  EXPECT_TRUE(os::exists(outputPath + taskName(index)));
}


// Tests falling back on bypassing the cache when fetching the download
// size of a URI that is supposed to be cached fails.
TEST_F(FetcherCacheTest, CachedFallback)
{
  startSlave();
  driver->start();

  // Make sure the content-length request fails.
  ASSERT_SOME(os::rm(commandPath));

  CommandInfo::URI uri;
  uri.set_value(commandPath);
  uri.set_executable(true);
  uri.set_cache(true);

  CommandInfo commandInfo;
  commandInfo.set_value("./" + COMMAND_NAME + " " + taskName(0));
  commandInfo.add_uris()->CopyFrom(uri);

  // Bring back the asset just before running mesos-fetcher to fetch it.
  Future<FetcherInfo> fetcherInfo;
  EXPECT_CALL(*fetcherProcess, run(_, _, _, _))
    .WillOnce(DoAll(FutureArg<3>(&fetcherInfo),
                    InvokeWithoutArgs(this,
                                      &FetcherCacheTest::setupCommandFileAsset),
                    Invoke(fetcherProcess,
                           &MockFetcherProcess::unmocked_run)));

  const Try<Task> task = launchTask(commandInfo, 0);
  ASSERT_SOME(task);

  AWAIT_READY(awaitFinished(task.get()));

  const string path = path::join(task->runDirectory.string(), COMMAND_NAME);
  EXPECT_TRUE(isExecutable(path));
  EXPECT_TRUE(os::exists(path + taskName(0)));

  AWAIT_READY(fetcherInfo);

  ASSERT_EQ(1, fetcherInfo->items_size());
  EXPECT_EQ(FetcherInfo::Item::BYPASS_CACHE,
            fetcherInfo->items(0).action());

  EXPECT_EQ(0u, fetcherProcess->cacheSize());
  ASSERT_SOME(fetcherProcess->cacheFiles());
  EXPECT_TRUE(fetcherProcess->cacheFiles()->empty());

  verifyCacheMetrics();
}


// Tests archive extraction without caching as a baseline for the
// subsequent test below.
TEST_F(FetcherCacheTest, LocalUncachedExtract)
{
  startSlave();
  driver->start();

  const int index = 0;
  CommandInfo::URI uri;
  uri.set_value(archivePath);
  uri.set_extract(true);

  CommandInfo commandInfo;
  commandInfo.set_value("./" + ARCHIVED_COMMAND_NAME + " " + taskName(index));
  commandInfo.add_uris()->CopyFrom(uri);

  const Try<Task> task = launchTask(commandInfo, index);
  ASSERT_SOME(task);

  AWAIT_READY(awaitFinished(task.get()));

  EXPECT_TRUE(os::exists(
      path::join(task->runDirectory.string(), ARCHIVE_NAME)));
  EXPECT_FALSE(isExecutable(
      path::join(task->runDirectory.string(), ARCHIVE_NAME)));

  const string path =
    path::join(task->runDirectory.string(), ARCHIVED_COMMAND_NAME);
  EXPECT_TRUE(isExecutable(path));
  EXPECT_TRUE(os::exists(path + taskName(index)));

  EXPECT_EQ(0u, fetcherProcess->cacheSize());
  ASSERT_SOME(fetcherProcess->cacheFiles());
  EXPECT_TRUE(fetcherProcess->cacheFiles()->empty());

  verifyCacheMetrics();
}


// Tests archive extraction in combination with caching.
TEST_F(FetcherCacheTest, LocalCachedExtract)
{
  startSlave();
  driver->start();

  for (size_t i = 0; i < 2; i++) {
    CommandInfo::URI uri;
    uri.set_value(archivePath);
    uri.set_extract(true);
    uri.set_cache(true);

    CommandInfo commandInfo;
    commandInfo.set_value("./" + ARCHIVED_COMMAND_NAME + " " + taskName(i));
    commandInfo.add_uris()->CopyFrom(uri);

    const Try<Task> task = launchTask(commandInfo, i);
    ASSERT_SOME(task);

    AWAIT_READY(awaitFinished(task.get()));

    EXPECT_FALSE(os::exists(
        path::join(task->runDirectory.string(), ARCHIVE_NAME)));

    const string path =
      path::join(task->runDirectory.string(), ARCHIVED_COMMAND_NAME);
    EXPECT_TRUE(isExecutable(path));
    EXPECT_TRUE(os::exists(path + taskName(i)));

    EXPECT_EQ(1u, fetcherProcess->cacheSize());
    ASSERT_SOME(fetcherProcess->cacheFiles());
    EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

    verifyCacheMetrics();
  }
}


class FetcherCacheHttpTest : public FetcherCacheTest
{
public:
  // A minimal HTTP server (NOTE: not written as an actor, but this is
  // deprecated, see below) just reusing what is already implemented
  // somewhere to serve some HTTP requests for file downloads. Plus
  // counting how many requests are made. Plus the ability to pause
  // answering requests, stalling them.
  //
  // TODO(bernd-mesos): This class follows a dangerous style of mixing
  // actors and non-actors, DO NOT REPLICATE. Ultimately we want to
  // replace this with a generic HTTP server that can be used by other
  // tests as well and enables things like pausing requests,
  // manipulating requests, mocking, etc.
  class HttpServer : public Process<HttpServer>
  {
  public:
  public:
    HttpServer(const string& _commandPath, const string& _archivePath)
      : countRequests(0),
        countCommandRequests(0),
        countArchiveRequests(0),
        commandPath(_commandPath),
        archivePath(_archivePath)
    {
      CHECK(!_commandPath.empty());
      CHECK(!_archivePath.empty());
    }

    void initialize() override
    {
      provide(COMMAND_NAME, commandPath);
      provide(ARCHIVE_NAME, archivePath);
    }

    string url()
    {
      return "http://" + stringify(self().address) + "/" + self().id + "/";
    }

    // Stalls the execution of future HTTP requests inside consume().
    void pause()
    {
      // If there is no latch or if the existing latch has already been
      // triggered, create a new latch.
      if (latch.get() == nullptr || latch->await(Duration::min())) {
        latch.reset(new Latch());
      }
    }

    void resume()
    {
      if (latch.get() != nullptr) {
        latch->trigger();
      }
    }

    void consume(HttpEvent&& event) override
    {
      if (latch.get() != nullptr) {
        latch->await();
      }

      countRequests++;

      if (strings::contains(event.request->url.path, COMMAND_NAME)) {
        countCommandRequests++;
      }

      if (strings::contains(event.request->url.path, ARCHIVE_NAME)) {
        countArchiveRequests++;
      }

      ProcessBase::consume(std::move(event));
    }

    void resetCounts()
    {
      countRequests = 0;
      countCommandRequests = 0;
      countArchiveRequests = 0;
    }

    size_t countRequests;
    size_t countCommandRequests;
    size_t countArchiveRequests;

  private:
    const string commandPath;
    const string archivePath;
    Owned<Latch> latch;
  };

  void SetUp() override
  {
    FetcherCacheTest::SetUp();

    httpServer = new HttpServer(commandPath, archivePath);
    spawn(httpServer);
  }

  void TearDown() override
  {
    terminate(httpServer);
    wait(httpServer);
    delete httpServer;

    FetcherCacheTest::TearDown();
  }

  HttpServer* httpServer;
};


// Tests fetching via HTTP with caching. Only one download must
// occur. Fetching is serialized, to cover code areas without
// overlapping/concurrent fetch attempts.
TEST_F(FetcherCacheHttpTest, HttpCachedSerialized)
{
  startSlave();
  driver->start();

  for (size_t i = 0; i < 3; i++) {
    CommandInfo::URI uri;
    uri.set_value(httpServer->url() + COMMAND_NAME);
    uri.set_executable(true);
    uri.set_cache(true);

    CommandInfo commandInfo;
    commandInfo.set_value("./" + COMMAND_NAME + " " + taskName(i));
    commandInfo.add_uris()->CopyFrom(uri);

    const Try<Task> task = launchTask(commandInfo, i);
    ASSERT_SOME(task);

    AWAIT_READY(awaitFinished(task.get()));

    const string path =
      path::join(task->runDirectory.string(), COMMAND_NAME);
    EXPECT_TRUE(isExecutable(path));
    EXPECT_TRUE(os::exists(path + taskName(i)));

    EXPECT_EQ(1u, fetcherProcess->cacheSize());
    ASSERT_SOME(fetcherProcess->cacheFiles());
    EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

    verifyCacheMetrics();

    // 2 requests: 1 for content-length, 1 for download.
    EXPECT_EQ(2u, httpServer->countCommandRequests);
  }
}


// Tests multiple concurrent fetching efforts that require some
// concurrency control. One task must "win" and perform the size
// and download request for the URI alone. The others must reuse
// the result.
TEST_F(FetcherCacheHttpTest, HttpCachedConcurrent)
{
  startSlave();
  driver->start();

  // Causes fetch contention. No task can run yet until resume().
  httpServer->pause();

  vector<CommandInfo> commandInfos;
  const size_t countTasks = 5;

  for (size_t i = 0; i < countTasks; i++) {
    CommandInfo::URI uri0;
    uri0.set_value(httpServer->url() + COMMAND_NAME);
    uri0.set_executable(true);
    uri0.set_cache(true);

    CommandInfo commandInfo;
    commandInfo.set_value("./" + COMMAND_NAME + " " + taskName(i));
    commandInfo.add_uris()->CopyFrom(uri0);

    // Not always caching this URI causes that it will be downloaded
    // some of the time. Thus we exercise code paths that eagerly fetch
    // new assets while waiting for pending downloads of cached assets
    // as well as code paths where no downloading occurs at all.
    if (i % 2 == 1) {
      CommandInfo::URI uri1;
      uri1.set_value(httpServer->url() + ARCHIVE_NAME);
      commandInfo.add_uris()->CopyFrom(uri1);
    }

    commandInfos.push_back(commandInfo);
  }

  Try<vector<Task>> tasks = launchTasks(commandInfos);
  ASSERT_SOME(tasks);

  ASSERT_EQ(countTasks, tasks->size());

  // Having paused the HTTP server, ensure that FetcherProcess::_fetch()
  // has been called for each task, which means that all tasks are competing
  // for downloading the same URIs.
  foreach (const Owned<Promise<Nothing>>& waypoint, fetchContentionWaypoints) {
    AWAIT(waypoint->future());
  }

  // Now let the tasks run.
  httpServer->resume();

  AWAIT_READY(awaitFinished(tasks.get()));

  EXPECT_EQ(1u, fetcherProcess->cacheSize());
  ASSERT_SOME(fetcherProcess->cacheFiles());
  EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

  verifyCacheMetrics();

  // HTTP requests regarding the archive asset as follows. Archive
  // "content-length" requests: 1, archive file downloads: 2.
  EXPECT_EQ(2u, httpServer->countCommandRequests);

  // HTTP requests regarding the command asset as follows. Command
  // "content-length" requests: 0, command file downloads: 2.
  EXPECT_EQ(2u, httpServer->countArchiveRequests);

  for (size_t i = 0; i < countTasks; i++) {
    EXPECT_EQ(i % 2 == 1, os::exists(
        path::join(tasks->at(i).runDirectory.string(), ARCHIVE_NAME)));
    EXPECT_TRUE(isExecutable(
        path::join(tasks->at(i).runDirectory.string(), COMMAND_NAME)));
    EXPECT_TRUE(os::exists(
        path::join(tasks->at(i).runDirectory.string(),
                   COMMAND_NAME + taskName(i))));
  }
}


// Tests using multiple URIs per command, variations of caching,
// setting the executable flag, and archive extraction.
TEST_F(FetcherCacheHttpTest, HttpMixed)
{
  startSlave();
  driver->start();

  // Causes fetch contention. No task can run yet until resume().
  httpServer->pause();

  vector<CommandInfo> commandInfos;

  // Task 0.

  CommandInfo::URI uri00;
  uri00.set_value(httpServer->url() + ARCHIVE_NAME);
  uri00.set_cache(true);
  uri00.set_extract(false);
  uri00.set_executable(false);

  CommandInfo::URI uri01;
  uri01.set_value(httpServer->url() + COMMAND_NAME);
  uri01.set_extract(false);
  uri01.set_executable(true);

  CommandInfo commandInfo0;
  commandInfo0.set_value("./" + COMMAND_NAME + " " + taskName(0));
  commandInfo0.add_uris()->CopyFrom(uri00);
  commandInfo0.add_uris()->CopyFrom(uri01);
  commandInfos.push_back(commandInfo0);

  // Task 1.

  CommandInfo::URI uri10;
  uri10.set_value(httpServer->url() + ARCHIVE_NAME);
  uri10.set_extract(true);
  uri10.set_executable(false);

  CommandInfo::URI uri11;
  uri11.set_value(httpServer->url() + COMMAND_NAME);
  uri11.set_extract(true);
  uri11.set_executable(false);

  CommandInfo commandInfo1;
  commandInfo1.set_value("./" + ARCHIVED_COMMAND_NAME + " " + taskName(1));
  commandInfo1.add_uris()->CopyFrom(uri10);
  commandInfo1.add_uris()->CopyFrom(uri11);
  commandInfos.push_back(commandInfo1);

  // Task 2.

  CommandInfo::URI uri20;
  uri20.set_value(httpServer->url() + ARCHIVE_NAME);
  uri20.set_cache(true);
  uri20.set_extract(true);
  uri20.set_executable(false);

  CommandInfo::URI uri21;
  uri21.set_value(httpServer->url() + COMMAND_NAME);
  uri21.set_extract(false);
  uri21.set_executable(false);

  CommandInfo commandInfo2;
  commandInfo2.set_value("./" + ARCHIVED_COMMAND_NAME + " " + taskName(2));
  commandInfo2.add_uris()->CopyFrom(uri20);
  commandInfo2.add_uris()->CopyFrom(uri21);
  commandInfos.push_back(commandInfo2);

  Try<vector<Task>> tasks = launchTasks(commandInfos);
  ASSERT_SOME(tasks);

  ASSERT_EQ(3u, tasks->size());

  // Having paused the HTTP server, ensure that FetcherProcess::_fetch()
  // has been called for each task, which means that all tasks are competing
  // for downloading the same URIs.
  foreach (const Owned<Promise<Nothing>>& waypoint, fetchContentionWaypoints) {
    AWAIT(waypoint->future());
  }

  // Now let the tasks run.
  httpServer->resume();

  AWAIT_READY(awaitFinished(tasks.get()));

  EXPECT_EQ(1u, fetcherProcess->cacheSize());
  ASSERT_SOME(fetcherProcess->cacheFiles());
  EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

  verifyCacheMetrics();

  // HTTP requests regarding the command asset as follows. Command
  // "content-length" requests: 0, command file downloads: 3.
  EXPECT_EQ(3u, httpServer->countCommandRequests);

  // HTTP requests regarding the archive asset as follows. Archive
  // "content-length" requests: 1, archive file downloads: 2.
  EXPECT_EQ(3u, httpServer->countArchiveRequests);

  // Task 0.

  EXPECT_FALSE(isExecutable(
      path::join(tasks->at(0).runDirectory.string(), ARCHIVE_NAME)));
  EXPECT_FALSE(os::exists(
      path::join(tasks->at(0).runDirectory.string(), ARCHIVED_COMMAND_NAME)));

  EXPECT_TRUE(isExecutable(
      path::join(tasks->at(0).runDirectory.string(), COMMAND_NAME)));
  EXPECT_TRUE(os::exists(
      path::join(tasks->at(0).runDirectory.string(),
                 COMMAND_NAME + taskName(0))));

  // Task 1.

  EXPECT_FALSE(isExecutable(path::join(
      tasks->at(1).runDirectory.string(),
      ARCHIVE_NAME)));
  EXPECT_TRUE(isExecutable(path::join(
      tasks->at(1).runDirectory.string(),
      ARCHIVED_COMMAND_NAME)));
  EXPECT_TRUE(os::exists(path::join(
      tasks->at(1).runDirectory.string(),
      ARCHIVED_COMMAND_NAME + taskName(1))));

  EXPECT_FALSE(isExecutable(path::join(
      tasks->at(1).runDirectory.string(),
      COMMAND_NAME)));

  // Task 2.

  EXPECT_FALSE(os::exists(path::join(
      tasks->at(2).runDirectory.string(),
      ARCHIVE_NAME)));
  EXPECT_TRUE(isExecutable(path::join(
      tasks->at(2).runDirectory.string(),
      ARCHIVED_COMMAND_NAME)));
  EXPECT_TRUE(os::exists(path::join(
      tasks->at(2).runDirectory.string(),
      ARCHIVED_COMMAND_NAME + taskName(2))));

  EXPECT_FALSE(isExecutable(path::join(
      tasks->at(2).runDirectory.string(),
      COMMAND_NAME)));
}


// Tests slave recovery of the fetcher cache. The cache must be
// wiped clean on recovery, causing renewed downloads.
// TODO(bernd-mesos): Debug flaky behavior reported in MESOS-2871,
// then reenable this test.
TEST_F(FetcherCacheHttpTest, DISABLED_HttpCachedRecovery)
{
  startSlave();
  driver->start();

  for (size_t i = 0; i < 3; i++) {
    CommandInfo::URI uri;
    uri.set_value(httpServer->url() + COMMAND_NAME);
    uri.set_executable(true);
    uri.set_cache(true);

    CommandInfo commandInfo;
    commandInfo.set_value("./" + COMMAND_NAME + " " + taskName(i));
    commandInfo.add_uris()->CopyFrom(uri);

    const Try<Task> task = launchTask(commandInfo, i);
    ASSERT_SOME(task);

    AWAIT_READY(awaitFinished(task.get()));

    const string path = path::join(task->runDirectory.string(), COMMAND_NAME);
    EXPECT_TRUE(isExecutable(path));
    EXPECT_TRUE(os::exists(path + taskName(i)));

    EXPECT_EQ(1u, fetcherProcess->cacheSize());
    ASSERT_SOME(fetcherProcess->cacheFiles());
    EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

    // content-length requests: 1
    // downloads: 1
    EXPECT_EQ(2u, httpServer->countCommandRequests);
  }

  // Stop and destroy the current slave.
  slave->terminate();

  // Start over.
  httpServer->resetCounts();

  // Don't reuse the old fetcher, which has stale state after
  // stopping the slave.
  Fetcher fetcher2(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher2);

  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  // Set up so we can wait until the new slave updates the container's
  // resources (this occurs after the executor has reregistered).
  Future<Nothing> update =
    FUTURE_DISPATCH(_, &MesosContainerizerProcess::update);

  Try<Owned<cluster::Slave>> _slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(_slave);
  slave = _slave.get();

  // Wait until the containerizer is updated.
  AWAIT_READY(update);

  // Repeat of the above to see if it works the same.
  for (size_t i = 0; i < 3; i++) {
    CommandInfo::URI uri;
    uri.set_value(httpServer->url() + COMMAND_NAME);
    uri.set_executable(true);
    uri.set_cache(true);

    CommandInfo commandInfo;
    commandInfo.set_value("./" + COMMAND_NAME + " " + taskName(i));
    commandInfo.add_uris()->CopyFrom(uri);

    const Try<Task> task = launchTask(commandInfo, i);
    ASSERT_SOME(task);

    AWAIT_READY(awaitFinished(task.get()));

    const string path =
      path::join(task->runDirectory.string(), COMMAND_NAME);
    EXPECT_TRUE(isExecutable(path));
    EXPECT_TRUE(os::exists(path + taskName(i)));

    EXPECT_EQ(1u, fetcherProcess->cacheSize());
    ASSERT_SOME(fetcherProcess->cacheFiles());
    EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

    verifyCacheMetrics();

    // content-length requests: 1
    // downloads: 1
    EXPECT_EQ(2u, httpServer->countCommandRequests);
  }
}


// Tests cache eviction. Limits the available cache space then fetches
// more task scripts than fit into the cache and runs them all. We
// observe how the number of cache files rises and then stays constant.
TEST_F(FetcherCacheTest, SimpleEviction)
{
  const size_t countCacheEntries = 2;

  // Let only the first 'countCacheEntries' downloads fit in the cache.
  flags.fetcher_cache_size = COMMAND_SCRIPT.size() * countCacheEntries;

  startSlave();
  driver->start();

  for (size_t i = 0; i < countCacheEntries + 2; i++) {
    string commandFilename = "cmd" + stringify(i);
    string command = commandFilename + " " + taskName(i);

    commandPath = path::join(assetsDirectory, commandFilename);
    ASSERT_SOME(os::write(commandPath, COMMAND_SCRIPT));

    CommandInfo::URI uri;
    uri.set_value(commandPath);
    uri.set_executable(true);
    uri.set_cache(true);

    CommandInfo commandInfo;
    commandInfo.set_value("./" + command);
    commandInfo.add_uris()->CopyFrom(uri);

    const Try<Task> task = launchTask(commandInfo, i);
    ASSERT_SOME(task);

    AWAIT_READY(awaitFinished(task.get()));

    // Check that the task succeeded.
    EXPECT_TRUE(isExecutable(
        path::join(task->runDirectory.string(), commandFilename)));
    EXPECT_TRUE(os::exists(
        path::join(task->runDirectory.string(), COMMAND_NAME + taskName(i))));

    if (i < countCacheEntries) {
      EXPECT_EQ(i + 1, fetcherProcess->cacheSize());
      ASSERT_SOME(fetcherProcess->cacheFiles());
      EXPECT_EQ(i+1u, fetcherProcess->cacheFiles()->size());
    } else {
      EXPECT_EQ(countCacheEntries, fetcherProcess->cacheSize());
      ASSERT_SOME(fetcherProcess->cacheFiles());
      EXPECT_EQ(countCacheEntries,
                fetcherProcess->cacheFiles()->size());
    }
  }

  verifyCacheMetrics();
}


// Tests cache eviction fallback to bypassing the cache. A first task
// runs normally. Then a second succeeds using eviction. Then a third
// task fails to evict, but still gets executed bypassing the cache.
TEST_F(FetcherCacheTest, FallbackFromEviction)
{
  // The size by which every task's URI download is going to be larger
  // than the previous one.
  const size_t growth = 10;

  // Let only the first two downloads fit into the cache, one at a time,
  // the second evicting the first. The third file won't fit any more,
  // being larger than the entire cache.
  flags.fetcher_cache_size = COMMAND_SCRIPT.size() + growth;

  startSlave();
  driver->start();

  // We'll run 3 tasks and these are the task completion futures to wait
  // for each time.
  Future<FetcherInfo> fetcherInfo0;
  Future<FetcherInfo> fetcherInfo1;
  Future<FetcherInfo> fetcherInfo2;
  EXPECT_CALL(*fetcherProcess, run(_, _, _, _))
    .WillOnce(DoAll(FutureArg<3>(&fetcherInfo0),
                    Invoke(fetcherProcess,
                           &MockFetcherProcess::unmocked_run)))
    .WillOnce(DoAll(FutureArg<3>(&fetcherInfo1),
                    Invoke(fetcherProcess,
                           &MockFetcherProcess::unmocked_run)))
    .WillOnce(DoAll(FutureArg<3>(&fetcherInfo2),
                    Invoke(fetcherProcess,
                           &MockFetcherProcess::unmocked_run)));


  // Task 0:

  const string commandFilename0 = "cmd0";
  const string command0 = commandFilename0 + " " + taskName(0);

  commandPath = path::join(assetsDirectory, commandFilename0);

  // Write the command into the script that gets fetched.
  ASSERT_SOME(os::write(commandPath, COMMAND_SCRIPT));

  CommandInfo::URI uri0;
  uri0.set_value(commandPath);
  uri0.set_executable(true);
  uri0.set_cache(true);

  CommandInfo commandInfo0;
  commandInfo0.set_value("./" + command0);
  commandInfo0.add_uris()->CopyFrom(uri0);

  const Try<Task> task0 = launchTask(commandInfo0, 0);
  ASSERT_SOME(task0) << task0.error();

  AWAIT_READY(awaitFinished(task0.get()));

  // Check that the task succeeded.
  EXPECT_TRUE(isExecutable(
      path::join(task0->runDirectory.string(), commandFilename0)));
  EXPECT_TRUE(os::exists(
      path::join(task0->runDirectory.string(), COMMAND_NAME + taskName(0))));

  AWAIT_READY(fetcherInfo0);

  ASSERT_EQ(1, fetcherInfo0->items_size());
  EXPECT_EQ(FetcherInfo::Item::DOWNLOAD_AND_CACHE,
            fetcherInfo0->items(0).action());

  // We have put a file of size 'COMMAND_SCRIPT.size()' in the cache
  // with space 'COMMAND_SCRIPT.size() + growth'. So we must have 'growth'
  // space left.
  ASSERT_EQ(Bytes(growth), fetcherProcess->availableCacheSpace());

  EXPECT_EQ(1u, fetcherProcess->cacheSize());
  ASSERT_SOME(fetcherProcess->cacheFiles());
  EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

  verifyCacheMetrics();

  // Task 1:

  const string commandFilename1 = "cmd1";
  const string command1 = commandFilename1 + " " + taskName(1);

  commandPath = path::join(assetsDirectory, commandFilename1);

  // Write the command into the script that gets fetched. Add 'growth'
  // extra characters so the cache will fill up to the last byte.
  ASSERT_SOME(os::write(
      commandPath,
      COMMAND_SCRIPT + string(growth, '\n')));

  CommandInfo::URI uri1;
  uri1.set_value(commandPath);
  uri1.set_executable(true);
  uri1.set_cache(true);

  CommandInfo commandInfo1;
  commandInfo1.set_value("./" + command1);
  commandInfo1.add_uris()->CopyFrom(uri1);

  const Try<Task> task1 = launchTask(commandInfo1, 1);
  ASSERT_SOME(task1) << task1.error();

  AWAIT_READY(awaitFinished(task1.get()));

  // Check that the task succeeded.
  EXPECT_TRUE(isExecutable(
      path::join(task1->runDirectory.string(), commandFilename1)));
  EXPECT_TRUE(os::exists(
      path::join(task1->runDirectory.string(), COMMAND_NAME + taskName(1))));

  AWAIT_READY(fetcherInfo1);

  ASSERT_EQ(1, fetcherInfo1->items_size());
  EXPECT_EQ(FetcherInfo::Item::DOWNLOAD_AND_CACHE,
            fetcherInfo1->items(0).action());

  // The cache must now be full.
  ASSERT_EQ(Bytes(0u), fetcherProcess->availableCacheSpace());

  EXPECT_EQ(1u, fetcherProcess->cacheSize());
  ASSERT_SOME(fetcherProcess->cacheFiles());
  EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

  verifyCacheMetrics();

  // Task 2:

  const string commandFilename2 = "cmd2";
  const string command2 = commandFilename2 + " " + taskName(2);

  commandPath = path::join(assetsDirectory, commandFilename2);

  // Write the command into the script that gets fetched. Add
  // '2 * growth' now. Thus the file will be so big that it will not
  // fit into the cache any more.
  ASSERT_SOME(os::write(
      commandPath,
      COMMAND_SCRIPT + string(2 * growth, '\n')));

  CommandInfo::URI uri2;
  uri2.set_value(commandPath);
  uri2.set_executable(true);
  uri2.set_cache(true);

  CommandInfo commandInfo2;
  commandInfo2.set_value("./" + command2);
  commandInfo2.add_uris()->CopyFrom(uri2);

  const Try<Task> task2 = launchTask(commandInfo2, 2);
  ASSERT_SOME(task2) << task2.error();

  AWAIT_READY(awaitFinished(task2.get()));

  // Check that the task succeeded.
  EXPECT_TRUE(isExecutable(
      path::join(task2->runDirectory.string(), commandFilename2)));
  EXPECT_TRUE(os::exists(
      path::join(task2->runDirectory.string(), COMMAND_NAME + taskName(2))));

  AWAIT_READY(fetcherInfo2);

  ASSERT_EQ(1, fetcherInfo2->items_size());
  EXPECT_EQ(FetcherInfo::Item::BYPASS_CACHE,
            fetcherInfo2->items(0).action());

  EXPECT_EQ(1u, fetcherProcess->cacheSize());
  ASSERT_SOME(fetcherProcess->cacheFiles());
  EXPECT_EQ(1u, fetcherProcess->cacheFiles()->size());

  verifyCacheMetrics();
}


// Tests LRU cache eviction strategy.
TEST_F(FetcherCacheTest, RemoveLRUCacheEntries)
{
  // Let only two downloads fit in the cache.
  flags.fetcher_cache_size = COMMAND_SCRIPT.size() * 2;

  startSlave();
  driver->start();

  // Start commands using a pattern that will fill the cache with two entries
  // and request the second entry again. The first entry is then the LRU.
  // Adding a new entry should therefore evict the first entry.
  vector<int> commandCreationPattern;
  commandCreationPattern.push_back(0);
  commandCreationPattern.push_back(1);
  commandCreationPattern.push_back(1);
  commandCreationPattern.push_back(2);

  int taskIndex = 0;

  // Fill up the cache
  foreach (const int i, commandCreationPattern) {
    string commandFilename = "cmd" + stringify(i);
    string command = commandFilename + " " + taskName(taskIndex);

    commandPath = path::join(assetsDirectory, commandFilename);
    ASSERT_SOME(os::write(commandPath, COMMAND_SCRIPT));

    CommandInfo::URI uri;
    uri.set_value(commandPath);
    uri.set_executable(true);
    uri.set_cache(true);

    CommandInfo commandInfo;
    commandInfo.set_value("./" + command);
    commandInfo.add_uris()->CopyFrom(uri);

    const Try<Task> task = launchTask(commandInfo, taskIndex);
    ASSERT_SOME(task);

    AWAIT_READY(awaitFinished(task.get()));

    // Check that the task succeeded.
    EXPECT_TRUE(isExecutable(
        path::join(task->runDirectory.string(), commandFilename)));
    EXPECT_TRUE(os::exists(path::join(task->runDirectory.string(),
                                      COMMAND_NAME + taskName(taskIndex))));

    ++taskIndex;
  }

  EXPECT_EQ(2u, fetcherProcess->cacheSize());

  verifyCacheMetrics();

  // FetcherProcess::cacheFiles returns all cache files that are in the cache
  // directory. We expect cmd1 and cmd2 to be there, cmd0 should have been
  // evicted.
  Try<list<Path>> cacheFiles = fetcherProcess->cacheFiles();
  ASSERT_SOME(cacheFiles);

  bool cmd1Found = false;
  bool cmd2Found = false;

  foreach (const Path& cacheFile, cacheFiles.get()) {
    if (strings::contains(cacheFile.basename(), "cmd1")) {
      cmd1Found = true;
    }

    if (strings::contains(cacheFile.basename(), "cmd2")) {
      cmd2Found = true;
    }
  }

  EXPECT_TRUE(cmd1Found);
  EXPECT_TRUE(cmd2Found);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
