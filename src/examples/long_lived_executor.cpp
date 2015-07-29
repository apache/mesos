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

#include <stdlib.h> // For random.

#include <cstdlib>
#include <iostream>
#include <thread>

#include <mesos/executor.hpp>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>

using namespace mesos;

using std::cout;
using std::endl;
using std::string;


void run(ExecutorDriver* driver, const TaskInfo& task)
{
  os::sleep(Seconds(random() % 10));

  TaskStatus status;
  status.mutable_task_id()->MergeFrom(task.task_id());
  status.set_state(TASK_FINISHED);

  driver->sendStatusUpdate(status);
}


class LongLivedExecutor : public Executor
{
public:
  virtual ~LongLivedExecutor() {}

  virtual void registered(ExecutorDriver* driver,
                          const ExecutorInfo& executorInfo,
                          const FrameworkInfo& frameworkInfo,
                          const SlaveInfo& slaveInfo)
  {
    cout << "Registered executor on " << slaveInfo.hostname() << endl;
  }

  virtual void reregistered(ExecutorDriver* driver,
                            const SlaveInfo& slaveInfo)
  {
    cout << "Re-registered executor on " << slaveInfo.hostname() << endl;
  }

  virtual void disconnected(ExecutorDriver* driver) {}

  virtual void launchTask(ExecutorDriver* driver, const TaskInfo& task)
  {
    cout << "Starting task " << task.task_id().value() << endl;

    std::thread* thread = new std::thread([=]() {
      run(driver, task);
    });

    thread->detach();

    TaskStatus status;
    status.mutable_task_id()->MergeFrom(task.task_id());
    status.set_state(TASK_RUNNING);

    driver->sendStatusUpdate(status);
  }

  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId) {}
  virtual void frameworkMessage(ExecutorDriver* driver, const string& data) {}
  virtual void shutdown(ExecutorDriver* driver) {}
  virtual void error(ExecutorDriver* driver, const string& message) {}
};


int main(int argc, char** argv)
{
  LongLivedExecutor executor;
  MesosExecutorDriver driver(&executor);
  return driver.run() == DRIVER_STOPPED ? 0 : 1;
}
