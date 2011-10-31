#!/usr/bin/env python
import mesos
import mesos_pb2
import sys
import time
import os
import atexit

from subprocess import *

def cleanup():
  try:
    # TODO(*): This will kill ALL mpds...oops.
    print "cleanup"
    os.waitpid(Popen("pkill -f /usr/local/bin/mpd", shell=True).pid, 0)
  except Exception, e:
    print e
    None

class MyExecutor(mesos.Executor):
  def init(self, driver, arg):
    [ip,port] = arg.data.split(":")
    self.ip = ip
    self.port = port

  def launchTask(self, driver, task):
    print "Running task %s" % task.task_id.value
    update = mesos_pb2.TaskStatus()
    update.task_id.value = task.task_id.value
    update.state = mesos_pb2.TASK_RUNNING
    driver.sendStatusUpdate(update)
    Popen("mpd -n -h "+self.ip+" -p "+self.port, shell=True)

  def killTask(self, driver, tid):
    # TODO(*): Kill only one of the mpd's!
    sys.exit(1)

  def shutdown(self, driver):
    print "shutdown"
    cleanup()

  def error(self, driver, code, message):
    print "Error: %s" % message

if __name__ == "__main__":
  print "Starting executor"
  atexit.register(cleanup)
  executor = MyExecutor()
  mesos.MesosExecutorDriver(executor).run()
