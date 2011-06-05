#!/usr/bin/env python
import nexus
import pickle
import time


class NestedExecutor(nexus.Executor):
  def __init__(self):
    nexus.Executor.__init__(self)
    self.tid = -1

  def init(self, driver, args):
    self.fid = args.frameworkId

  def launchTask(self, driver, task):
    self.tid = task.taskId
    duration = pickle.loads(task.arg)
    print "(%s:%d) Sleeping for %s seconds." % (self.fid, self.tid, duration)
    # TODO(benh): Don't sleep, this blocks the event loop!
    time.sleep(duration)
    status = nexus.TaskStatus(self.tid, nexus.TASK_FINISHED, "")
    driver.sendStatusUpdate(status)
    time.sleep(1)
    
  def killTask(self, driver, tid):
    if (self.tid != tid):
      print "Expecting different task id ... killing anyway!"
    status = nexus.TaskStatus(tid, nexus.TASK_FINISHED, "")
    driver.sendStatusUpdate(status)

  def error(self, driver, code, message):
    print "Error: %s" % message


if __name__ == "__main__":
  nexus.NexusExecutorDriver(NestedExecutor()).run()
