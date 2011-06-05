#!/usr/bin/env python
import mesos
import os
import sys
import time
import re
import threading

from optparse import OptionParser
from subprocess import *

TOTAL_TASKS = 1
MPI_TASK = ""
MPD_PID = ""
CPUS = 1
MEM = 1024

def mpiexec(driver):
  print "Got "+str(TOTAL_TASKS)+" mpd slots, running mpiexec"
  try:
    print "Running: "+"mpiexec -n "+str(TOTAL_TASKS)+" "+MPI_TASK
    os.system("mpiexec -1 -n "+str(TOTAL_TASKS)+" "+MPI_TASK)
  except OSError,e:
    print >>sys.stderr, "Error executing mpiexec"
    print >>sys.stderr, e
    exit(2)
  print "mpiexec completed, calling mpdexit "+MPD_PID
  call(["mpdexit",MPD_PID])
  driver.stop()

class MyScheduler(mesos.Scheduler):
  def __init__(self, ip, port):
    mesos.Scheduler.__init__(self)
    self.ip = ip
    self.port = port
    self.tasksLaunched = 0
    self.tasksFinished = 0

  def getFrameworkName(self, driver):
    return "Mesos MPI Framework"

  def getExecutorInfo(self, driver):
    execPath = os.path.join(os.getcwd(), "startmpd.sh")
    initArg = ip + ":" + port
    return mesos.ExecutorInfo(execPath, initArg)

  def registered(self, driver, fid):
    print "Mesos MPI scheduler and mpd running at "+self.ip+":"+self.port

  def resourceOffer(self, driver, oid, offers):
    print "Got offer %s" % oid
    tasks = []
    if self.tasksLaunched == TOTAL_TASKS:
      print "Rejecting permanently because we have already started"
      driver.replyToOffer(oid, tasks, {"timeout": "-1"})
      return
    for offer in offers:
      print "Considering slot on %s" % offer.host
      cpus = int(offer.params["cpus"])
      mem = int(offer.params["mem"])
      if cpus < CPUS or mem < MEM:
        print "Rejecting slot due to too few resources"
      elif self.tasksLaunched < TOTAL_TASKS:
        tid = self.tasksLaunched
        print "Accepting slot to start mpd %d" % tid
        params = {"cpus": "%d" % CPUS, "mem": "%d" % MEM}
        td = mesos.TaskDescription(
            tid, offer.slaveId, "task %d" % tid, params, "")
        tasks.append(td)
        self.tasksLaunched += 1
      else:
        print "Rejecting slot because we've launched enough tasks"
    driver.replyToOffer(oid, tasks, {"timeout": "1"})
    if self.tasksLaunched == TOTAL_TASKS:
      print "We've launched all our MPDs; waiting for them to come up"
      while countMPDs() <= TOTAL_TASKS:
        print "...waiting on MPD(s)..."
        time.sleep(1)
      threading.Thread(target = mpiexec, args=[driver]).start()

  def statusUpdate(self, driver, update):
    print "Task %d in state %d" % (update.taskId, update.state)
    if (update.state == mesos.TASK_FINISHED or
        update.state == mesos.TASK_FAILED or
        update.state == mesos.TASK_KILLED or
        update.state == mesos.TASK_LOST):
      print "A task finished unexpectedly, calling mpdexit "+MPD_PID
      call(["mpdexit",MPD_PID])
      driver.stop()

def countMPDs():
    try:
        mpdtraceout = Popen("mpdtrace -l", shell=True, stdout=PIPE).stdout
        count = 0
        for line in mpdtraceout:
            count += 1

        mpdtraceout.close()
        return count
    except OSError,e:
        print >>sys.stderr, "Error starting mpd or mpdtrace"
        print >>sys.stderr, e
        exit(2)

def parseIpPort(s):
  ba = re.search("_([^ ]*) \(([^)]*)\)", s)
  ip = ba.group(2)
  port = ba.group(1)
  return (ip,port)

if __name__ == "__main__":
  parser = OptionParser(usage="Usage: %prog [options] mesos_master mpi_program")
  parser.add_option("-n","--num",
                    help="number of slots/mpd:s to allocate (default 1)", 
                    dest="num", type="int", default=1)
  parser.add_option("-c","--cpus",
                    help="number of cpus per slot (default 1)",
                    dest="cpus", type="int", default=CPUS)
  parser.add_option("-m","--mem",
                    help="number of MB of memory per slot (default 1GB)",
                    dest="mem", type="int", default=MEM)

  (options,args)=parser.parse_args()
  if len(args)<2:
    print >>sys.stderr, "At least two parameters required."
    print >>sys.stderr, "Use --help to show usage."
    exit(2)

  TOTAL_TASKS = options.num
  CPUS = options.cpus
  MEM = options.mem
  MPI_TASK = " ".join(args[1:])

  print "Connecting to mesos master %s" % args[0]
 
  try:
    call(["mpd","--daemon"])
    mpdtraceout = Popen("mpdtrace -l", shell=True, stdout=PIPE).stdout
    traceline = mpdtraceout.readline()
  except OSError,e:
    print >>sys.stderr, "Error starting mpd or mpdtrace"
    print >>sys.stderr, e
    exit(2)

  (ip,port) = parseIpPort(traceline)

  MPD_PID = traceline.split(" ")[0]

  sched = MyScheduler(ip, port)
  mesos.MesosSchedulerDriver(sched, args[0]).run()
