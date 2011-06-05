#!/usr/bin/env python
import nexus
import sys
import time

class MyExecutor(nexus.Executor):
  def __init__(self):
    nexus.Executor.__init__(self)

  def startTask(self, driver, task):
    print "Running task %d" % task.taskId
    time.sleep(1)
    print "Sending the update..."
    update = nexus.TaskStatus(task.taskId, nexus.TASK_FINISHED, "")
    driver.sendStatusUpdate(update)
    print "Sent the update"

  def error(self, driver, code, message):
    print "Error: %s" % message

if __name__ == "__main__":
  print "Starting executor"
  nexus.NexusExecutorDriver(MyExecutor()).run()
