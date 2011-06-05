#!/usr/bin/env python
import nexus
import pickle
import time


class NestedExecutor(nexus.Executor):
  def __init__(self):
    nexus.Executor.__init__(self)
    self.tid = -1

  def init(self, args):
    self.fid = args.frameworkId

  def startTask(self, task):
    self.tid = task.taskId
    duration = pickle.loads(task.arg)
    print "(%d:%d) Sleeping for %s seconds." % (self.fid, self.tid, duration)
    # TODO(benh): Don't sleep, this blocks the event loop!
    time.sleep(duration)
    self.sendStatusUpdate(nexus.TaskStatus(self.tid, nexus.TASK_FINISHED, ""))

  def killTask(self, tid):
    if (self.tid != tid):
      print "Expecting different task id ... killing anyway!"
    self.sendStatusUpdate(nexus.TaskStatus(tid, nexus.TASK_FINISHED, ""))

  def error(self, code, message):
    print "Error: %s" % message


if __name__ == "__main__":
  NestedExecutor().run()
