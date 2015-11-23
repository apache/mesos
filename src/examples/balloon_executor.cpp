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

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>

#include <iostream>
#include <string>

#include <mesos/executor.hpp>

#include <stout/duration.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>

using namespace mesos;


// The amount of memory in MB each balloon step consumes.
const static size_t BALLOON_STEP_MB = 64;


// This function will increase the memory footprint gradually. The parameter
// limit specifies the upper limit (in MB) of the memory footprint. The
// parameter step specifies the step size (in MB).
static void balloon(size_t limit)
{
  size_t chunk = BALLOON_STEP_MB * 1024 * 1024;
  for (size_t i = 0; i < limit / BALLOON_STEP_MB; i++) {
    std::cout << "Increasing memory footprint by "
              << BALLOON_STEP_MB << " MB" << std::endl;

    // Allocate page-aligned virtual memory.
    void* buffer = NULL;
    if (posix_memalign(&buffer, getpagesize(), chunk) != 0) {
      perror("Failed to allocate page-aligned memory, posix_memalign");
      abort();
    }

    // We use memset and mlock here to make sure that the memory
    // actually gets paged in and thus accounted for.
    if (memset(buffer, 1, chunk) != buffer) {
      perror("Failed to fill memory, memset");
      abort();
    }

    if (mlock(buffer, chunk) != 0) {
      perror("Failed to lock memory, mlock");
      abort();
    }

    // Try not to increase the memory footprint too fast.
    os::sleep(Seconds(1));
  }
}


class BalloonExecutor : public Executor
{
public:
  virtual ~BalloonExecutor() {}

  virtual void registered(ExecutorDriver* driver,
                          const ExecutorInfo& executorInfo,
                          const FrameworkInfo& frameworkInfo,
                          const SlaveInfo& slaveInfo)
  {
    std::cout << "Registered" << std::endl;
  }

  virtual void reregistered(ExecutorDriver* driver,
                            const SlaveInfo& slaveInfo)
  {
    std::cout << "Reregistered" << std::endl;
  }

  virtual void disconnected(ExecutorDriver* driver)
  {
    std::cout << "Disconnected" << std::endl;
  }

  virtual void launchTask(ExecutorDriver* driver, const TaskInfo& task)
  {
    std::cout << "Starting task " << task.task_id().value() << std::endl;

    TaskStatus status;
    status.mutable_task_id()->MergeFrom(task.task_id());
    status.set_state(TASK_RUNNING);

    driver->sendStatusUpdate(status);

    // Get the balloon limit (in MB).
    Try<size_t> limit = numify<size_t>(task.data());
    assert(limit.isSome());
    size_t balloonLimit = limit.get();

    // Artificially increase the memory usage gradually. The
    // balloonLimit specifies the upper limit. The balloonLimit can be
    // larger than the amount of memory allocated to this executor. In
    // that case, the isolator (e.g. cgroups) should be able to detect
    // that and the task should not be able to reach TASK_FINISHED
    // state.
    balloon(balloonLimit);

    std::cout << "Finishing task " << task.task_id().value() << std::endl;

    status.mutable_task_id()->MergeFrom(task.task_id());
    status.set_state(TASK_FINISHED);

    driver->sendStatusUpdate(status);
  }

  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId)
  {
    std::cout << "Kill task " << taskId.value() << std::endl;
  }

  virtual void frameworkMessage(ExecutorDriver* driver, const std::string& data)
  {
    std::cout << "Framework message: " << data << std::endl;
  }

  virtual void shutdown(ExecutorDriver* driver)
  {
    std::cout << "Shutdown" << std::endl;
  }

  virtual void error(ExecutorDriver* driver, const std::string& message)
  {
    std::cout << "Error message: " << message << std::endl;
  }
};


int main(int argc, char** argv)
{
  BalloonExecutor executor;
  MesosExecutorDriver driver(&executor);
  return driver.run() == DRIVER_STOPPED ? 0 : 1;
}
