#!/usr/bin/env python
import nexus
import os
import sys
import time

TOTAL_TASKS = 5

TASK_CPUS = 1
TASK_MEM = 32 * 1024 * 1024

class MyScheduler(nexus.Scheduler):
  def __init__(self):
    nexus.Scheduler.__init__(self) # Required to extend Scheduler in Python
    self.tasksLaunched = 0
    self.tasksFinished = 0

  def getFrameworkName(self, driver):
    return "Python test framework"

  def getExecutorInfo(self, driver):
    execPath = os.path.join(os.getcwd(), "test_exec.sh")
    return nexus.ExecutorInfo(execPath, "")

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
        td = nexus.TaskDescription(tid, offer.slaveId, "task %d" % tid,
            params, "")
        tasks.append(td)
    driver.replyToOffer(oid, tasks, {})

  def statusUpdate(self, driver, update):
    print "Task %d is in state %d" % (update.taskId, update.state)
    if update.state == nexus.TASK_FINISHED:
      self.tasksFinished += 1
      if self.tasksFinished == TOTAL_TASKS:
        print "All tasks done, exiting"
        driver.stop()

if __name__ == "__main__":
  print "Connecting to %s" % sys.argv[1]
  nexus.NexusSchedulerDriver(MyScheduler(), sys.argv[1]).run()
