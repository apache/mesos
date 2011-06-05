#!/usr/bin/env python
import os
import pickle
import re
import sys
import time
from optparse import OptionParser

import mesos

# Default resources to use for the command we execute.
DEFAULT_CPUS = 1
DEFAULT_MEM = 512


# The scheduler for mesos-submit, running on the machine that the user
# executed mesos-submit on, launches a single task in the cluster with
# the required amounts of CPUs and memory, and then waits for it to become
# the framework's scheduler using the scheduler failover mechanism.
# It then exits mesos-submit successfully, while the task goes on to run
# the user's command.
#
# Note that we pass our framework ID, master URL and command to the executor
# using the task's argument field.
#
# We currently don't recover if our task fails for some reason, but we
# do print its state transitions so the user can notice this.
class SubmitScheduler(mesos.Scheduler):
  def __init__(self, options, master, command):
    mesos.Scheduler.__init__(self)
    if options.name != None:
      self.framework_name = options.name
    else:
      self.framework_name = "mesos-submit " + command
    self.cpus = options.cpus
    self.mem = options.mem
    self.master = master
    self.command = command
    self.task_launched = False

  def getFrameworkName(self, driver):
    return self.framework_name

  def getExecutorInfo(self, driver):
    frameworkDir = os.path.abspath(os.path.dirname(sys.argv[0]))
    executorPath = os.path.join(frameworkDir, "executor")
    return mesos.ExecutorInfo(executorPath, "")

  def registered(self, driver, fid):
    print "Registered with Mesos, FID = %s" % fid
    self.fid = "" + fid

  def resourceOffer(self, driver, oid, offers):
    if self.task_launched:
      # Since we already launched our task, we reject the offer
      driver.replyToOffer(oid, [], {"timeout": "-1"})
    else:
      for offer in offers:
        cpus = int(offer.params["cpus"])
        mem = int(offer.params["mem"])
        if cpus >= self.cpus and mem >= self.mem:
          print "Accepting slot on slave %s (%s)" % (offer.slaveId, offer.host)
          params = {"cpus": "%d" % self.cpus, "mem": "%d" % self.mem}
          arg = [self.fid, self.framework_name, self.master, self.command]
          task = mesos.TaskDescription(0, offer.slaveId, "task", params,
                                       pickle.dumps(arg))
          driver.replyToOffer(oid, [task], {"timeout": "1"})
          self.task_launched = True
          return

  def statusUpdate(self, driver, update):
    print "Task %d in state %d" % (update.taskId, update.state)

  def error(self, driver, code, message):
    if message == "Framework failover":
      # Scheduler failover is currently reported by this error message;
      # this is kind of a brittle way to detect it, but it's all we can do now.
      print "In-cluster scheduler started; exiting mesos-submit"
    else:
      print "Error from Mesos: %s (error code: %d)" % (message, code)
    driver.stop()


if __name__ == "__main__":
  parser = OptionParser(usage="Usage: %prog [options] <master_url> <command>")
  parser.add_option("-c","--cpus",
                    help="number of CPUs to request (default: 1)",
                    dest="cpus", type="int", default=DEFAULT_CPUS)
  parser.add_option("-m","--mem",
                    help="MB of memory to request (default: 512)",
                    dest="mem", type="int", default=DEFAULT_MEM)
  parser.add_option("-n","--name",
                    help="Framework name", dest="name", type="string")
  (options,args) = parser.parse_args()
  if len(args) < 2:
    parser.error("At least two parameters are required.")
    exit(2)
  master = args[0]
  command = " ".join(args[1:])
  print "Connecting to mesos master %s" % master
  sched = SubmitScheduler(options, master, command)
  mesos.MesosSchedulerDriver(sched, master).run()
