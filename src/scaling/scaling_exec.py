#!/usr/bin/env python
import mesos
import os
import pickle
import sys

CPUS = 1
MEM = 50*1024*1024

class NestedScheduler(mesos.Scheduler):
  def __init__(self, todo, duration, executor):
    mesos.Scheduler.__init__(self)
    self.tid = 0
    self.todo = todo
    self.finished = 0
    self.duration = duration
    self.executor = executor

  def getFrameworkName(self, driver):
    return "Nested Framework: %d todo at %d secs" % (self.todo, self.duration)

  def getExecutorInfo(self, driver):
    execPath = os.path.join(os.getcwd(), "nested_exec")
    return mesos.ExecutorInfo(execPath, "")

  def registered(self, driver, fid):
    print "Nested Scheduler Registered!"

  def resourceOffer(self, driver, oid, offers):
    tasks = []
    for offer in offers:
      if self.todo != self.tid:
        self.tid += 1
        pars = {"cpus": "%d" % CPUS, "mem": "%d" % MEM}
        task = mesos.TaskDescription(self.tid, offer.slaveId,
                                     "task %d" % self.tid, pars,
                                     pickle.dumps(self.duration))
        tasks.append(task)
        #msg = mesos.FrameworkMessage(-1, , "")
        #executor.sendFrameworkMessage("")
    driver.replyToOffer(oid, tasks, {})

  def statusUpdate(self, driver, status):
    if status.state == mesos.TASK_FINISHED:
      self.finished += 1
    if self.finished == self.todo:
      print "All nested tasks done, stopping scheduler and enclosing executor!"
      driver.stop()
      self.executor.stop()


class ScalingExecutor(mesos.Executor):
  def __init__(self):
    mesos.Executor.__init__(self)
    self.tid = -1
    self.nested_driver = -1

  def launchTask(self, driver, task):
    self.tid = task.taskId
    master, (todo, duration) = pickle.loads(task.arg)
    scheduler = NestedScheduler(todo, duration, self)
    print "Running here:" + master
    self.nested_driver = mesos.MesosSchedulerDriver(scheduler, master)
    self.nested_driver.start()
    
  def killTask(self, driver, tid):
    if (tid != self.tid):
      print "Expecting different task id ... killing anyway!"
    if self.nested_driver != -1:
      self.nested_driver.stop()
      self.nested_driver.join()
    driver.sendStatusUpdate(mesos.TaskStatus(tid, mesos.TASK_FINISHED, ""))

  def shutdown(self, driver):
    self.killTask(self.tid)

  def error(self, driver, code, message):
    print "Error: %s" % message


if __name__ == "__main__":
  mesos.MesosExecutorDriver(ScalingExecutor()).run()
