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

class TestScheduler(mesos.Scheduler):
  def __init__(self, executor):
    self.executor = executor
    self.tasksLaunched = 0
    self.tasksFinished = 0

  def registered(self, driver, frameworkId, masterInfo):
    print "Registered with framework ID %s" % frameworkId.value

  def resourceOffers(self, driver, offers):
    print "Got %d resource offers" % len(offers)
    for offer in offers:
      tasks = []
      print "Got resource offer %s" % offer.id.value
      if self.tasksLaunched < TOTAL_TASKS:
        tid = self.tasksLaunched
        self.tasksLaunched += 1

        print "Accepting offer on %s to start task %d" % (offer.hostname, tid)

        task = mesos_pb2.TaskInfo()
        task.task_id.value = str(tid)
        task.slave_id.value = offer.slave_id.value
        task.name = "task %d" % tid
        task.executor.MergeFrom(self.executor)

        cpus = task.resources.add()
        cpus.name = "cpus"
        cpus.type = mesos_pb2.Value.SCALAR
        cpus.scalar.value = TASK_CPUS

        mem = task.resources.add()
        mem.name = "mem"
        mem.type = mesos_pb2.Value.SCALAR
        mem.scalar.value = TASK_MEM

        tasks.append(task)
        driver.launchTasks(offer.id, tasks)

  def statusUpdate(self, driver, update):
    print "Task %s is in state %d" % (update.task_id.value, update.state)
    if update.state == mesos_pb2.TASK_FINISHED:
      self.tasksFinished += 1
      if self.tasksFinished == TOTAL_TASKS:
        print "All tasks done, exiting"
        driver.stop()

if __name__ == "__main__":
  if len(sys.argv) != 2:
    print "Usage: %s master" % sys.argv[0]
    sys.exit(1)

  executor = mesos_pb2.ExecutorInfo()
  executor.executor_id.value = "default"
  executor.command.value = os.path.abspath("./test-executor")

  framework = mesos_pb2.FrameworkInfo()
  framework.user = "" # Have Mesos fill in the current user.
  framework.name = "Test Framework (Python)"

  driver = mesos.MesosSchedulerDriver(
    TestScheduler(executor),
    framework,
    sys.argv[1])

  sys.exit(0 if driver.run() == mesos_pb2.DRIVER_STOPPED else 1)
