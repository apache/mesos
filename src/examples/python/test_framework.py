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

import os
import sys
import time

import mesos
import mesos_pb2

TOTAL_TASKS = 5

TASK_CPUS = 1
TASK_MEM = 32

class MyScheduler(mesos.Scheduler):
  def __init__(self):
    self.tasksLaunched = 0
    self.tasksFinished = 0

  def registered(self, driver, fid):
    print "Registered with framework ID %s" % fid.value

  def resourceOffers(self, driver, offers):
    print "Got %d resource offers" % len(offers)
    for offer in offers:
      tasks = []
      print "Got resource offer %s" % offer.id.value
      if self.tasksLaunched < TOTAL_TASKS:
        tid = self.tasksLaunched
        self.tasksLaunched += 1

        print "Accepting offer on %s to start task %d" % (offer.hostname, tid)

        task = mesos_pb2.TaskDescription()
        task.task_id.value = str(tid)
        task.slave_id.value = offer.slave_id.value
        task.name = "task %d" % tid

        cpus = task.resources.add()
        cpus.name = "cpus"
        cpus.type = mesos_pb2.Resource.SCALAR
        cpus.scalar.value = TASK_CPUS

        mem = task.resources.add()
        mem.name = "mem"
        mem.type = mesos_pb2.Resource.SCALAR
        mem.scalar.value = TASK_MEM

        tasks.append(task)
        driver.launchTasks(offer.id, tasks)

  def statusUpdate(self, driver, update):
    print "Task %s is in state %d" % (update.task_id.value, update.state)
    if update.state == mesos_pb2.TASK_FINISHED:
      self.tasksFinished += 1
      if self.tasksFinished == TOTAL_TASKS:
        print "All tasks done, exiting"
        driver.stop(False)

if __name__ == "__main__":
  print "Connecting to %s" % sys.argv[1]

  frameworkDir = os.path.abspath(os.path.dirname(sys.argv[0]))
  execPath = os.path.join(frameworkDir, "test_executor")
  execInfo = mesos_pb2.ExecutorInfo()
  execInfo.executor_id.value = "default"
  execInfo.uri = execPath

  sys.exit(mesos.MesosSchedulerDriver(MyScheduler(), "Python test framework",
                                      execInfo, sys.argv[1]).run())
