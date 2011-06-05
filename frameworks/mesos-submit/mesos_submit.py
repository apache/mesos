#!/usr/bin/env python
import os
import re
import sys
import time
from optparse import OptionParser

import mesos

# Default resources to use for the command we execute.
DEFAULT_CPUS = 1
DEFAULT_MEM = 512


class SubmitScheduler(mesos.Scheduler):
  def __init__(self, cpus, mem, master, command):
    mesos.Scheduler.__init__(self)
    self.cpus = cpus
    self.mem = mem
    self.master = master
    self.command = command
    self.task_launched = False

  def getFrameworkName(self, driver):
    print "In getFrameworkName"
    return "mesos-submit " + self.command

  def getExecutorInfo(self, driver):
    print "In getExecutorInfo"
    executorPath = os.path.join(os.getcwd(), "executor")
    return mesos.ExecutorInfo(executorPath, "")

  def registered(self, driver, fid):
    print "Registered with Mesos, FID = %s" % fid
    self.fid = "" + fid

  def resourceOffer(self, driver, oid, offers):
    if self.task_launched:
      driver.replyToOffer(oid, [], {"timeout": "-1"})
    else:
      for offer in offers:
        cpus = int(offer.params["cpus"])
        mem = int(offer.params["mem"])
        if cpus >= self.cpus and mem >= self.mem:
          print "Accepting slot on slave %s (%s)" % (offer.slaveId, offer.host)
          params = {"cpus": "%d" % self.cpus, "mem": "%d" % self.mem}
          arg = "%s|%s|%s" % (self.fid, self.master, self.command)
          task = mesos.TaskDescription(0, offer.slaveId, "task", params, arg)
          driver.replyToOffer(oid, [task], {"timeout": "1"})
          self.task_launched = True
          return

  def statusUpdate(self, driver, update):
    print "Task %d in state %d" % (update.taskId, update.state)

  def error(self, driver, code, message):
    if message == "Framework failover":
      print "Secondary scheduler registered successfully; exiting mesos-submit"
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
  (options,args)= parser.parse_args()
  if len(args) < 2:
    parser.error("At least two parameters are required.")
    exit(2)
  master = args[0]
  command = " ".join(args[1:])
  print "Connecting to mesos master %s" % master
  sched = SubmitScheduler(options.cpus, options.mem, master, command)
  mesos.MesosSchedulerDriver(sched, master).run()
