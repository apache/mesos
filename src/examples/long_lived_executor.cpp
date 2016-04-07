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


class LongLivedExecutor : public Executor
{
public:
  virtual void launch(ExecutorDriver* driver, const TaskInfo& task)
  {
    cout << "Starting task " << task.task_id().value() << endl;

    std::thread thread([=]() {
      os::sleep(Seconds(random() % 10));

      TaskStatus status;
      status.mutable_task_id()->MergeFrom(task.task_id());
      status.set_state(TASK_FINISHED);

      driver->sendStatusUpdate(status);
    });

    thread.detach();

    TaskStatus status;
    status.mutable_task_id()->MergeFrom(task.task_id());
    status.set_state(TASK_RUNNING);

    driver->sendStatusUpdate(status);
  }
};


int main(int argc, char** argv)
{
  LongLivedExecutor executor;
  MesosExecutorDriver driver(&executor);
  return driver.run() == DRIVER_STOPPED ? 0 : 1;
}
