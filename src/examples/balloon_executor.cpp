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

#include <glog/logging.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>

#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <mesos/executor.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/os.hpp>

#include <stout/os/pagesize.hpp>

using namespace mesos;


// Helper function to check whether the system uses swap.
//
// TODO(bbannier): Augment this implementation to explicitly check for swap
// on platforms other than Linux as well. It might make sense to move this
// function to stout once the implementation becomes more complete.
Try<bool> hasSwap() {
  // We read `/proc/swaps` and check whether any active swap
  // partitions are listed. The format of that file is e.g.,
  //
  //     Filename  Type      Size       Used Priority
  //     /dev/sda4 partition 1234567890 348  -1
  //     /dev/sdb4 partition 1234567890 0    -2
  //
  // If we find more than one line (including the header) the
  // system likely uses swap.
  int numberOfLines = 0;

  std::ifstream swaps("/proc/swaps");
  std::string line;

  while (std::getline(swaps, line)) {
    ++numberOfLines;
  }

  // Check for read errors. This provides a default implementation
  // for platforms without proc filesystem as well.
  if (!swaps.eof()) {
    return Error("Could not determine whether this system uses swap");
  }

  return numberOfLines > 1;
}


// The amount of memory in MB each balloon step consumes.
const static size_t BALLOON_STEP_MB = 64;


// This function will increase the memory footprint gradually.
// `TaskInfo.data` specifies the upper limit (in MB) of the memory
// footprint. The upper limit can be larger than the amount of memory
// allocated to this executor. In that case, the isolator (e.g. cgroups)
// may detect that and destroy the container before the executor can
// sent a TASK_FINISHED update.
void run(ExecutorDriver* driver, const TaskInfo& task)
{
  TaskStatus status;
  status.mutable_task_id()->MergeFrom(task.task_id());
  status.set_state(TASK_RUNNING);

  driver->sendStatusUpdate(status);

  // Get the balloon limit (in MB).
  Try<Bytes> _limit = Bytes::parse(task.data());
  CHECK(_limit.isSome());
  const size_t limit = _limit->bytes() / Bytes::MEGABYTES;

  // On systems with swap partitions we explicitly prevent memory used by the
  // executor from being swapped out with `mlock`. Since the amount of memory a
  // process can lock is controlled by an rlimit, we only `mlock` when
  // strictly necessary to prevent complicating the test setup.
  Try<bool> hasSwap_ = hasSwap();
  const bool lockMemory = hasSwap_.isError() || hasSwap_.get();

  if (lockMemory) {
    LOG(INFO)
      << "System might use swap partitions, will explicitly"
      << " lock memory to prevent swapping";
  }

  const size_t chunk = BALLOON_STEP_MB * 1024 * 1024;
  for (size_t i = 0; i < limit / BALLOON_STEP_MB; i++) {
    LOG(INFO)
      << "Increasing memory footprint by " << BALLOON_STEP_MB << " MB";

    // Allocate page-aligned virtual memory.
    void* buffer = nullptr;
    if (posix_memalign(&buffer, os::pagesize(), chunk) != 0) {
      LOG(FATAL) << ErrnoError(
          "Failed to allocate page-aligned memory, posix_memalign").message;
    }

    // We use `memset` and possibly `mlock` here to make sure that the
    // memory actually gets paged in and thus accounted for.
    if (memset(buffer, 1, chunk) != buffer) {
      LOG(FATAL) << ErrnoError("Failed to fill memory, memset").message;
    }

    if (lockMemory) {
      if (mlock(buffer, chunk) != 0) {
        LOG(FATAL) << ErrnoError("Failed to lock memory, mlock").message;
      }
    }

    // Try not to increase the memory footprint too fast.
    os::sleep(Seconds(1));
  }

  LOG(INFO) << "Finishing task " << task.task_id().value();

  status.set_state(TASK_FINISHED);

  driver->sendStatusUpdate(status);

  // This is a hack to ensure the message is sent to the
  // slave before we exit the process. Without this, we
  // may exit before libprocess has sent the data over
  // the socket. See MESOS-4111.
  os::sleep(Seconds(1));
  driver->stop();
}


class BalloonExecutor : public Executor
{
public:
  BalloonExecutor()
    : launched(false) {}

  ~BalloonExecutor() override {}

  void registered(ExecutorDriver* driver,
                          const ExecutorInfo& executorInfo,
                          const FrameworkInfo& frameworkInfo,
                          const SlaveInfo& slaveInfo) override
  {
    LOG(INFO) << "Registered";
  }

  void reregistered(ExecutorDriver* driver,
                            const SlaveInfo& slaveInfo) override
  {
    LOG(INFO) << "Reregistered";
  }

  void disconnected(ExecutorDriver* driver) override
  {
    LOG(INFO) << "Disconnected";
  }

  void launchTask(ExecutorDriver* driver, const TaskInfo& task) override
  {
    if (launched) {
      TaskStatus status;
      status.mutable_task_id()->MergeFrom(task.task_id());
      status.set_state(TASK_FAILED);
      status.set_message(
          "Attempted to run multiple balloon tasks with the same executor");

      driver->sendStatusUpdate(status);
      return;
    }

    LOG(INFO) << "Starting task " << task.task_id().value();

    // NOTE: The executor driver calls `launchTask` synchronously, which
    // means that calls such as `driver->sendStatusUpdate()` will not execute
    // until `launchTask` returns.
    std::thread thread([=]() {
      run(driver, task);
    });

    thread.detach();

    launched = true;
  }

  void killTask(ExecutorDriver* driver, const TaskID& taskId) override
  {
    LOG(INFO) << "Kill task " << taskId.value();
  }

  void frameworkMessage(
      ExecutorDriver* driver, const std::string& data) override
  {
    LOG(INFO) << "Framework message: " << data;
  }

  void shutdown(ExecutorDriver* driver) override
  {
    LOG(INFO) << "Shutdown";
  }

  void error(ExecutorDriver* driver, const std::string& message) override
  {
    LOG(INFO) << "Error message: " << message;
  }

private:
  bool launched;
};


int main(int argc, char** argv)
{
  BalloonExecutor executor;
  MesosExecutorDriver driver(&executor);
  return driver.run() == DRIVER_STOPPED ? 0 : 1;
}
