#!/usr/bin/env python

import mesos.interface
import mesos.native
from mesos.interface import mesos_pb2
import os
import sys
import time
import re
import threading

from optparse import OptionParser
from subprocess import *


def mpiexec():
  print "We've launched all our MPDs; waiting for them to come up"

  while countMPDs() <= TOTAL_MPDS:
    print "...waiting on MPD(s)..."
    time.sleep(1)
  print "Got %d mpd(s), running mpiexec" % TOTAL_MPDS

  try:
    print "Running mpiexec"
    call([MPICH2PATH + 'mpiexec', '-1', '-n', str(TOTAL_MPDS)] + MPI_PROGRAM)

  except OSError,e:
    print >> sys.stderr, "Error executing mpiexec"
    print >> sys.stderr, e
    exit(2)

  print "mpiexec completed, calling mpdallexit %s" % MPD_PID

  # Ring/slave mpd daemons will be killed on executor's shutdown() if
  # framework scheduler fails to call 'mpdallexit'.
  call([MPICH2PATH + 'mpdallexit', MPD_PID])


class MPIScheduler(mesos.interface.Scheduler):

  def __init__(self, options, ip, port):
    self.mpdsLaunched = 0
    self.mpdsFinished = 0
    self.ip = ip
    self.port = port
    self.options = options
    self.startedExec = False

  def registered(self, driver, fid, masterInfo):
    print "Mesos MPI scheduler and mpd running at %s:%s" % (self.ip, self.port)
    print "Registered with framework ID %s" % fid.value

  def resourceOffers(self, driver, offers):
    print "Got %d resource offers" % len(offers)

    for offer in offers:
      print "Considering resource offer %s from %s" % (offer.id.value, offer.hostname)

      if self.mpdsLaunched == TOTAL_MPDS:
        print "Declining permanently because we have already launched enough tasks"
        driver.declineOffer(offer.id)
        continue

      cpus = 0
      mem = 0
      tasks = []

      for resource in offer.resources:
        if resource.name == "cpus":
          cpus = resource.scalar.value
        elif resource.name == "mem":
          mem = resource.scalar.value

      if cpus < CPUS or mem < MEM:
        print "Declining offer due to too few resources"
        driver.declineOffer(offer.id)
      else:
        tid = self.mpdsLaunched
        self.mpdsLaunched += 1

        print "Accepting offer on %s to start mpd %d" % (offer.hostname, tid)

        task = mesos_pb2.TaskInfo()
        task.task_id.value = str(tid)
        task.slave_id.value = offer.slave_id.value
        task.name = "task %d " % tid

        cpus = task.resources.add()
        cpus.name = "cpus"
        cpus.type = mesos_pb2.Value.SCALAR
        cpus.scalar.value = CPUS

        mem = task.resources.add()
        mem.name = "mem"
        mem.type = mesos_pb2.Value.SCALAR
        mem.scalar.value = MEM

        task.command.value = "%smpd --noconsole --ncpus=%d --host=%s --port=%s" % (MPICH2PATH, CPUS, self.ip, self.port)

        tasks.append(task)

        print "Replying to offer: launching mpd %d on host %s" % (tid, offer.hostname)
        driver.launchTasks(offer.id, tasks)

    if not self.startedExec and self.mpdsLaunched == TOTAL_MPDS:
      threading.Thread(target = mpiexec).start()
      self.startedExec = True

  def statusUpdate(self, driver, update):
    print "Task %s in state %s" % (update.task_id.value, update.state)
    if (update.state == mesos_pb2.TASK_FAILED or
        update.state == mesos_pb2.TASK_KILLED or
        update.state == mesos_pb2.TASK_LOST):
      print "A task finished unexpectedly, calling mpdexit on %s" % MPD_PID
      call([MPICH2PATH + "mpdexit", MPD_PID])
      driver.stop()
    if (update.state == mesos_pb2.TASK_FINISHED):
      self.mpdsFinished += 1
      if self.mpdsFinished == TOTAL_MPDS:
        print "All tasks done, all mpd's closed, exiting"
        driver.stop()


def countMPDs():
  try:
    mpdtraceproc = Popen(MPICH2PATH + "mpdtrace -l", shell=True, stdout=PIPE)
    mpdtraceline = mpdtraceproc.communicate()[0]
    return mpdtraceline.count("\n")
  except OSError,e:
    print >>sys.stderr, "Error starting mpd or mpdtrace"
    print >>sys.stderr, e
    exit(2)


def parseIpPort(s):
  ba = re.search("([^_]*)_([0-9]*)", s)
  ip = ba.group(1)
  port = ba.group(2)
  return (ip, port)


if __name__ == "__main__":
  parser = OptionParser(usage="Usage: %prog [options] mesos_master mpi_program")
  parser.disable_interspersed_args()
  parser.add_option("-n", "--num",
                    help="number of mpd's to allocate (default 1)",
                    dest="num", type="int", default=1)
  parser.add_option("-c", "--cpus",
                    help="number of cpus per mpd (default 1)",
                    dest="cpus", type="int", default=1)
  parser.add_option("-m","--mem",
                    help="number of MB of memory per mpd (default 1GB)",
                    dest="mem", type="int", default=1024)
  parser.add_option("--name",
                    help="framework name", dest="name", type="string")
  parser.add_option("-p","--path",
                    help="path to look for MPICH2 binaries (mpd, mpiexec, etc.)",
                    dest="path", type="string", default="")
  parser.add_option("--ifhn-master",
                    help="alt. interface hostname for what mpd is running on (for scheduler)",
                    dest="ifhn_master", type="string")

  # Add options to configure cpus and mem.
  (options,args) = parser.parse_args()
  if len(args) < 2:
    print >> sys.stderr, "At least two parameters required."
    print >> sys.stderr, "Use --help to show usage."
    exit(2)

  TOTAL_MPDS = options.num
  CPUS = options.cpus
  MEM = options.mem
  MPI_PROGRAM = args[1:]

  # Give options.path a trailing '/', if it doesn't have one already.
  MPICH2PATH = os.path.join(options.path, "")

  print "Connecting to Mesos master %s" % args[0]

  try:
    mpd_cmd = MPICH2PATH + "mpd"
    mpdtrace_cmd = MPICH2PATH + "mpdtrace -l"

    if options.ifhn_master is not None:
      call([mpd_cmd, "--daemon", "--ifhn=" + options.ifhn_master])
    else:
      call([mpd_cmd, "--daemon"])

    mpdtraceproc = Popen(mpdtrace_cmd, shell=True, stdout=PIPE)
    mpdtraceout = mpdtraceproc.communicate()[0]

  except OSError,e:
    print >> sys.stderr, "Error starting mpd or mpdtrace"
    print >> sys.stderr, e
    exit(2)

  (ip,port) = parseIpPort(mpdtraceout)

  MPD_PID = mpdtraceout.split(" ")[0]
  print "MPD_PID is %s" % MPD_PID

  scheduler = MPIScheduler(options, ip, port)

  framework = mesos_pb2.FrameworkInfo()
  framework.user = ""

  if options.name is not None:
    framework.name = options.name
  else:
    framework.name = "MPI: %s" % MPI_PROGRAM[0]

  driver = mesos.native.MesosSchedulerDriver(
    scheduler,
    framework,
    args[0])
  sys.exit(0 if driver.run() == mesos_pb2.DRIVER_STOPPED else 1)
