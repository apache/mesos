#!/usr/bin/env python
import sys
import threading
import time

import mesos
import mesos_pb2

class MyExecutor(mesos.Executor):
  def launchTask(self, driver, task):
    # Create a thread to run the task. Tasks should always be run in new
    # threads or processes, rather than inside launchTask itself.
    def run_task():
      print "Running task %s" % task.task_id.value
      update = mesos_pb2.TaskStatus()
      update.task_id.value = task.task_id.value
      update.state = mesos_pb2.TASK_RUNNING
      driver.sendStatusUpdate(update)

      time.sleep(1)

      print "Sending status update..."
      update = mesos_pb2.TaskStatus()
      update.task_id.value = task.task_id.value
      update.state = mesos_pb2.TASK_FINISHED
      driver.sendStatusUpdate(update)
      print "Sent status update"
    thread = threading.Thread(target=run_task)
    thread.start()

if __name__ == "__main__":
  print "Starting executor"
  mesos.MesosExecutorDriver(MyExecutor()).run()
