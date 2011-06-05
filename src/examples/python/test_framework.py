#!/usr/bin/env python
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

  def getFrameworkName(self, driver):
    return "Python test framework"

  def getExecutorInfo(self, driver):
    frameworkDir = os.path.abspath(os.path.dirname(sys.argv[0]))
    execPath = os.path.join(frameworkDir, "test_executor")
    execInfo = mesos_pb2.ExecutorInfo()
    execInfo.executor_id.value = "default"
    execInfo.uri = execPath
    return execInfo

  def registered(self, driver, fid):
    print "Registered with framework ID %s" % fid.value

  def resourceOffer(self, driver, oid, offers):
    tasks = []
    print "Got resource offer %s" % oid.value
    for offer in offers:
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
    driver.replyToOffer(oid, tasks, {})

  def statusUpdate(self, driver, update):
    print "Task %s is in state %d" % (update.task_id.value, update.state)
    if update.state == mesos_pb2.TASK_FINISHED:
      self.tasksFinished += 1
      if self.tasksFinished == TOTAL_TASKS:
        print "All tasks done, exiting"
        driver.stop()

if __name__ == "__main__":
  print "Connecting to %s" % sys.argv[1]
  mesos.MesosSchedulerDriver(MyScheduler(), sys.argv[1]).run()
