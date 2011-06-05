#!/usr/bin/env python
import mesos
import sys
import time

class MyExecutor(mesos.Executor):
  def __init__(self):
    mesos.Executor.__init__(self)

  def launchTask(self, driver, task):
    print "Running task %d" % task.taskId
    time.sleep(1)
    print "Sending the update..."
    update = mesos.TaskStatus(task.taskId, mesos.TASK_FINISHED, "")
    driver.sendStatusUpdate(update)
    print "Sent the update"

  def error(self, driver, code, message):
    print "Error: %s" % message

if __name__ == "__main__":
  print "Starting executor"
  mesos.MesosExecutorDriver(MyExecutor()).run()
