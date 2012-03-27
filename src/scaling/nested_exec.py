#!/usr/bin/env python

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
# 
#     http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import mesos
import pickle
import time


class NestedExecutor(mesos.Executor):
  def __init__(self):
    mesos.Executor.__init__(self)
    self.tid = -1

  def registered(self, driver, executorInfo, framworkInfo, slaveInfo):
    self.frameworkId = frameworkInfo.id;

  def launchTask(self, driver, task):
    self.tid = task.taskId
    duration = pickle.loads(task.arg)
    print "(%s:%d) Sleeping for %s seconds." % (self.fid, self.tid, duration)
    # TODO(benh): Don't sleep, this blocks the event loop!
    time.sleep(duration)
    status = mesos.TaskStatus(self.tid, mesos.TASK_FINISHED, "")
    driver.sendStatusUpdate(status)
    time.sleep(1)

  def killTask(self, driver, tid):
    if (self.tid != tid):
      print "Expecting different task id ... killing anyway!"
    status = mesos.TaskStatus(tid, mesos.TASK_FINISHED, "")
    driver.sendStatusUpdate(status)

  def error(self, driver, code, message):
    print "Error: %s" % message


if __name__ == "__main__":
  mesos.MesosExecutorDriver(NestedExecutor()).run()
