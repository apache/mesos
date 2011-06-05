#!/usr/bin/env python
import mesos
import os
import sys
import time

TOTAL_TASKS = 5

TASK_CPUS = 1
TASK_MEM = 32

class MyScheduler(mesos.Scheduler):
  def __init__(self):
    mesos.Scheduler.__init__(self) # Required to extend Scheduler in Python
    self.tasksLaunched = 0
    self.tasksFinished = 0

  def getFrameworkName(self, driver):
    return "Python test framework"

  def getExecutorInfo(self, driver):
    frameworkDir = os.path.abspath(os.path.dirname(sys.argv[0]))
    execPath = os.path.join(frameworkDir, "test_executor")
    return mesos.ExecutorInfo(execPath, "")

  def registered(self, driver, fid):
    print "Registered!"

  def resourceOffer(self, driver, oid, offers):
    tasks = []
    print "Got a resource offer!"
    for offer in offers:
      if self.tasksLaunched < TOTAL_TASKS:
        tid = self.tasksLaunched
        self.tasksLaunched += 1
        print "Accepting offer on %s to start task %d" % (offer.host, tid)
        params = {"cpus": "%d" % TASK_CPUS, "mem": "%d" % TASK_MEM}
        td = mesos.TaskDescription(tid, offer.slaveId, "task %d" % tid,
            params, "")
        tasks.append(td)
    driver.replyToOffer(oid, tasks, {})

  def statusUpdate(self, driver, update):
    print "Task %d is in state %d" % (update.taskId, update.state)
    if update.state == mesos.TASK_FINISHED:
      self.tasksFinished += 1
      if self.tasksFinished == TOTAL_TASKS:
        print "All tasks done, exiting"
        driver.stop()

if __name__ == "__main__":
  print "Connecting to %s" % sys.argv[1]
  mesos.MesosSchedulerDriver(MyScheduler(), sys.argv[1]).run()
