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

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>

#include <iostream>
#include <string>
#include <vector>

#include <mesos/executor.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>

using namespace mesos;

using std::string;
using std::vector;


// This function will increase the memory footprint gradually. The parameter
// limit specifies the upper limit of the memory footprint. The
// parameter step specifies the step size.
static void balloon(const Bytes& limit, const Bytes& step)
{
  for (size_t i = 0; i < limit.bytes() / step.bytes(); i++) {
    std::cout << "Increasing memory footprint by " << step << std::endl;

    // Allocate page-aligned virtual memory.
    void* buffer = NULL;
    if (posix_memalign(&buffer, getpagesize(), step.bytes()) != 0) {
      perror("Failed to allocate page-aligned memory, posix_memalign");
      abort();
    }

    // We use mlock and memset here to make sure that the memory
    // actually gets paged in and thus accounted for.
    if (mlock(buffer, step.bytes()) != 0) {
      perror("Failed to lock memory, mlock");
      abort();
    }

    if (memset(buffer, 1, step.bytes()) != buffer) {
      perror("Failed to fill memory, memset");
      abort();
    }

    // Try not to increase the memory footprint too fast.
    os::sleep(Milliseconds(50));
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

    // Get the balloon step and limit.
    vector<string> split = strings::split(task.data(), ",");

    Try<Bytes> step = Bytes::parse(split[0]);
    assert(step.isSome());

    Try<Bytes> limit = Bytes::parse(split[1]);
    assert(limit.isSome());

    // Artificially increase the memory usage gradually. The limit
    // can be larger than the amount of memory allocated to this
    // executor. In that case, the isolator (e.g. cgroups) should be
    // able to detect that and the task should not be able to reach
    // TASK_FINISHED state.
    balloon(limit.get(), step.get());

    std::cout << "Finishing task " << task.task_id().value() << std::endl;

    status.mutable_task_id()->MergeFrom(task.task_id());
    status.set_state(TASK_FINISHED);

    driver->sendStatusUpdate(status);
  }

  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId)
  {
    std::cout << "Kill task " << taskId.value() << std::endl;
  }

  virtual void frameworkMessage(ExecutorDriver* driver, const string& data)
  {
    std::cout << "Framework message: " << data << std::endl;
  }

  virtual void shutdown(ExecutorDriver* driver)
  {
    std::cout << "Shutdown" << std::endl;
  }

  virtual void error(ExecutorDriver* driver, const string& message)
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
