#!/usr/bin/env python

import mesos
import os
import sys
import time
import httplib
import Queue
import threading

from optparse import OptionParser
from subprocess import *
from socket import gethostname


class MyScheduler(mesos.Scheduler):
  def __init__(self, num_tasks, jar_url, jar_class, jar_args):
    mesos.Scheduler.__init__(self)
    self.lock = threading.RLock()
    self.task_count = 0
    self.num_tasks = num_tasks
    self.jar_url = jar_url
    self.jar_class = jar_class
    self.jar_args = jar_args

  def getExecutorInfo(self, driver):
    execPath = os.path.join(os.getcwd(), "daemon_executor.sh")
    return mesos.ExecutorInfo(execPath, "")

  def registered(self, driver, fid):
    print "Mesos daemon scheduler registered as framework #%s" % fid

  def getFrameworkName(self, driver):
    return "Python Deploy Jar"

  def resourceOffer(self, driver, oid, slave_offers):
    print "Got resource offer %s" % oid
    self.lock.acquire()
    tasks = []
    for offer in slave_offers:
      if int(self.task_count) > int(self.num_tasks): 
        print "Rejecting slot because we've launched enough tasks"
      elif int(offer.params['mem']) < 1024 or int(offer.params['cpus']) < 1:
        print "Rejecting slot because it is too small"
        print "It had mem=" + offer.params['mem']\
        + " and cpus=" + offer.params['cpus']
      else:
        print "accepting slot of size 1 cpu and 1024MB to launch a jar"
        params = {"cpus": "1", "mem": "1024"}
        task_args = self.jar_url + "\t" + self.jar_class + "\t" + self.jar_args
        print "task args are: " + task_args
        td = mesos.TaskDescription(
            self.task_count, offer.slaveId, "task %d" % self.task_count, params, task_args)
        tasks.append(td)
        print "incrementing self.task_count from " + str(self.task_count)
        print "self.num_tasks is " + str(self.num_tasks)
        self.task_count += 1
    driver.replyToOffer(oid, tasks, {"timeout": "1"})
    self.lock.release()

if __name__ == "__main__":
  args = sys.argv[1:len(sys.argv)]
  
  print "sched = MyScheduler(" + args[1] + ", "+  args[2]+ ", "+ args[3]+ ", "+ " ".join(args[4:len(args)])+")"

  sched = MyScheduler(args[1], args[2], args[3], " ".join(args[4:len(args)]))

  print "Connecting to mesos master %s" % args[0]

  mesos.MesosSchedulerDriver(sched, sys.argv[1]).run()

  print "Finished!"
