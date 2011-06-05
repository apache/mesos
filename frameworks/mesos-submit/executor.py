#!/usr/bin/env python
import os
import subprocess
import sys
import threading
import time
from threading import Thread

import mesos


# This function is called in its own thread to actually run the user's command.
# When it finishes, it shuts down the scheduler driver (disconnecting the 
# framework) and exits the program.
def run_command(command, driver):
  print "Running " + command
  equal_signs = "=" * 40
  print equal_signs
  try:
    code = os.system(command)
    print equal_signs
    print "Command completed with code %d" % code
  except OSError,e:
    print equal_signs
    print "os.system call failed, see stderr for details"
    print >>sys.stderr, "Error executing command"
    print >>sys.stderr, e
    driver.stop()
    sys.exit(2)
  driver.stop()
  sys.exit(0)


# A secondary scheduler registered for our framework with Mesos so that
# our first scheduler (on the machine that ran mesos-submit) can disconnect.
# This scheduler launches no further tasks but allows our one task to continue
# running in the cluster -- the task essentially becomes its own scheduler.
class SecondaryScheduler(mesos.Scheduler):
  def __init__(self, command):
    mesos.Scheduler.__init__(self)
    self.command = command
    print "here"

  def getFrameworkName(self, driver):
    print "here 2"
    return "mesos-submit " + self.command

  def getExecutorInfo(self, driver):
    print "here 3"
    executorPath = os.path.join(os.getcwd(), "executor")
    return mesos.ExecutorInfo(executorPath, "")

  def resourceOffer(self, driver, oid, offers):
    # Reject the offer with an infinite timeout, since we are here
    # only to serve as a second scheduler to keep the framework running
    driver.replyToOffer(oid, [], {"timeout": "-1"})

  def registered(self, driver, fid):
    print "Registered with Mesos; starting command"
    Thread(target=run_command, args=[self.command, driver]).start()

  def error(self, driver, code, message):
    print "Error from Mesos: %s (code %s)" % (message, code)


# This function is called in a separate thread to run our secondary scheduler;
# for some reason, things fail if we launch it from the executor's launchTask
# callback (this is likely to be SWIG/Python related).
def run_scheduler(command, master, fid):
  print "Starting secondary scheduler"
  sched = SecondaryScheduler(command)
  sched_driver = mesos.MesosSchedulerDriver(sched, master, fid)
  sched_driver.run()


# Executor class for mesos-submit. Expects to be given a single task
# to launch with a framework ID, master URL and command as parameters.
# Once this task is received, the executor registers as a scheduler for the
# framework by creating a SecondaryScheduler object, allowing the mesos-submit
# command on the user's machine to exit, and it starts the user's command
# on this cluster node as a subprocess.
class MyExecutor(mesos.Executor):
  def __init__(self):
    mesos.Executor.__init__(self)
    self.sched = None

  def launchTask(self, driver, task):
    if self.sched == None:
      print "Received task; going to register as scheduler"
      # Recover framework ID, master and command from task arg
      pieces = task.arg.split("|")
      fid = pieces[0]
      master = pieces[1]
      command = "|".join(pieces[2:]) # In case there are | characters in command
      print "Parsed parameters:"
      print "  framework ID = %s" % fid
      print "  master = %s" % master
      print "  command = %s" % command
      # Start our secondary scheduler in a different thread (for some reason,
      # this fails if we do it from the same thread.. probably due to some
      # SWIG Python interaction).
      Thread(target=run_scheduler, args=[command, master, fid]).start()
    else:
      print "Error: received a second task -- this should never happen!"

  def killTask(self, driver, tid):
    sys.exit(1)

  def error(self, driver, code, message):
    print "Error from Mesos: %s (code %s)" % (message, code)


if __name__ == "__main__":
  print "Starting mesos-submit executor"
  executor = MyExecutor()
  mesos.MesosExecutorDriver(executor).run()
