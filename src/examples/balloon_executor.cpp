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
  const size_t limit = _limit->megabytes();

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

    // We use memset and mlock here to make sure that the memory
    // actually gets paged in and thus accounted for.
    if (memset(buffer, 1, chunk) != buffer) {
      LOG(FATAL) << ErrnoError("Failed to fill memory, memset").message;
    }

    if (mlock(buffer, chunk) != 0) {
      LOG(FATAL) << ErrnoError("Failed to lock memory, mlock").message;
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

  virtual ~BalloonExecutor() {}

  virtual void registered(ExecutorDriver* driver,
                          const ExecutorInfo& executorInfo,
                          const FrameworkInfo& frameworkInfo,
                          const SlaveInfo& slaveInfo)
  {
    LOG(INFO) << "Registered";
  }

  virtual void reregistered(ExecutorDriver* driver,
                            const SlaveInfo& slaveInfo)
  {
    LOG(INFO) << "Reregistered";
  }

  virtual void disconnected(ExecutorDriver* driver)
  {
    LOG(INFO) << "Disconnected";
  }

  virtual void launchTask(ExecutorDriver* driver, const TaskInfo& task)
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

  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId)
  {
    LOG(INFO) << "Kill task " << taskId.value();
  }

  virtual void frameworkMessage(ExecutorDriver* driver, const std::string& data)
  {
    LOG(INFO) << "Framework message: " << data;
  }

  virtual void shutdown(ExecutorDriver* driver)
  {
    LOG(INFO) << "Shutdown";
  }

  virtual void error(ExecutorDriver* driver, const std::string& message)
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
