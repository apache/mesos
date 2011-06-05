#!/usr/bin/env python
import mesos
import pickle
import time


class NestedExecutor(mesos.Executor):
  def __init__(self):
    mesos.Executor.__init__(self)
    self.tid = -1

  def init(self, driver, args):
    self.fid = args.frameworkId

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
