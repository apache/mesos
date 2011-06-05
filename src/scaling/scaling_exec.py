#!/usr/bin/env python
import nexus
import os
import pickle
import sys


class NestedScheduler(nexus.Scheduler):
  def __init__(self, todo, duration, executor):
    nexus.Scheduler.__init__(self)
    self.tid = 0
    self.todo = todo
    self.finished = 0
    self.duration = duration
    self.executor = executor

  def getFrameworkName(self, driver):
    return "Nested Framework: %d todo at %d secs" % (self.todo, self.duration)

  def getExecutorInfo(self, driver):
    return nexus.ExecutorInfo("nested_exec", os.getcwd(), "")

  def registered(self, driver, fid):
    print "Nested Scheduler Registered!"

  def resourceOffer(self, driver, oid, offers):
    tasks = []
    for offer in offers:
      if self.todo != self.tid:
        self.tid += 1
        task = nexus.TaskDescription(self.tid, offer.slaveId,
                                     "task %d" % self.tid, offer.params,
                                     pickle.dumps(self.duration))
        tasks.append(task)
    driver.replyToOffer(oid, tasks, {})

  def statusUpdate(self, driver, status):
    if status.state == nexus.TASK_FINISHED:
      self.finished += 1
    if self.finished == self.todo:
      print "All nested tasks done, stopping scheduler and enclosing executor!"
      driver.stop()
      self.executor.stop()


class ScalingExecutor(nexus.Executor):
  def __init__(self):
    nexus.Executor.__init__(self)
    self.tid = -1
    self.driver = -1

  def startTask(self, task):
    self.tid = task.taskId
    master, (todo, duration) = pickle.loads(task.arg)
    scheduler = NestedScheduler(todo, duration, self)
    self.driver = nexus.NexusSchedulerDriver(scheduler, master)
    self.driver.start()

  def killTask(self, tid):
    if (tid != self.tid):
      print "Expecting different task id ... killing anyway!"
    if self.driver != -1:
      self.driver.stop()
      self.driver.join()
    self.sendStatusUpdate(nexus.TaskStatus(tid, nexus.TASK_FINISHED, ""))

  def shutdown(self):
    self.killTask(self.tid)

  def error(self, code, message):
    print "Error: %s" % message


if __name__ == "__main__":
  ScalingExecutor().run()
