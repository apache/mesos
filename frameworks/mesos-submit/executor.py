#!/usr/bin/env python
import os
import pickle
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
  def __init__(self, framework_name, command):
    mesos.Scheduler.__init__(self)
    self.framework_name = framework_name
    self.command = command
    self.command_started = False

  def getFrameworkName(self, driver):
    return self.framework_name

  def getExecutorInfo(self, driver):
    executorPath = os.path.join(os.getcwd(), "executor")
    return mesos.ExecutorInfo(executorPath, "")

  def resourceOffer(self, driver, oid, offers):
    # Reject the offer with an infinite timeout, since we are here
    # only to serve as a second scheduler to keep the framework running
    driver.replyToOffer(oid, [], {"timeout": "-1"})

  def registered(self, driver, fid):
    # Registered is called both the first time whe connect to a master
    # and subsequent times if we reconnect or the master fails over.
    # We only want to launch the command the first time.
    if not self.command_started:
      print "Registered with Mesos; starting command"
      Thread(target=run_command, args=[self.command, driver]).start()
      self.command_started = True
    else:
      print "Re-registered with Mesos (likely due to master failover)"

  def error(self, driver, code, message):
    print "Error from Mesos: %s (code %s)" % (message, code)


# This function is called in a separate thread to run our secondary scheduler;
# for some reason, things fail if we launch it from the executor's launchTask
# callback (this is likely to be SWIG/Python related).
def run_scheduler(fid, framework_name, master, command):
  print "Starting secondary scheduler"
  sched = SecondaryScheduler(framework_name, command)
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
      fid, framework_name, master, command = pickle.loads(task.arg)
      print "Mesos-submit parameters:"
      print "  framework ID = %s" % fid
      print "  framework name = %s" % framework_name
      print "  master = %s" % master
      print "  command = %s" % command
      # Start our secondary scheduler in a different thread (for some reason,
      # this fails if we do it from the same thread.. probably due to some
      # SWIG Python interaction).
      Thread(target=run_scheduler, 
             args=[fid, framework_name, master, command]).start()
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
