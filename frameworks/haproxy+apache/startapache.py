#!/usr/bin/env python
import mesos
import sys
import time
import os
import atexit

from subprocess import *

APACHECTL = "/usr/apache2/2.2/bin/apachectl" #EC2
#APACHECTL = "sudo /etc/init.d/apache2" #R Cluster

def cleanup():
  try:
    # TODO(*): This will kill ALL apaches...oops.
    os.waitpid(Popen(APACHECTL + " stop", shell=True).pid, 0)
  except Exception, e:
    print e
    None

class MyExecutor(mesos.Executor):
  def __init__(self):
    mesos.Executor.__init__(self)
    self.tid = -1

  def launchTask(self, driver, task):
    self.tid = task.taskId
    Popen(APACHECTL + " start", shell=True)

  def killTask(self, driver, tid):
    if (tid != self.tid):
      print "Expecting different task id ... killing anyway!"
    cleanup()
    update = mesos.TaskStatus(tid, mesos.TASK_FINISHED, "")
    driver.sendStatusUpdate(update)

  def shutdown(driver, self):
    cleanup()

  def error(self, code, message):
    print "Error: %s" % message

if __name__ == "__main__":
  print "Starting haproxy+apache executor"
  atexit.register(cleanup)
  executor = MyExecutor()
  mesos.MesosExecutorDriver(executor).run()
