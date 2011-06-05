#!/usr/bin/env python
import nexus
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

class MyExecutor(nexus.Executor):
  def __init__(self):
    nexus.Executor.__init__(self)

  def init(self, driver, arg):
    [ip,port] = arg.data.split(":")
    self.ip = ip
    self.port = port

  def startTask(self, driver, task):
    print "Running task %d" % task.taskId
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
  nexus.NexusExecutorDriver(executor).run()
