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

MIN_SERVERS = 1
START_THRESHOLD = 25
KILL_THRESHOLD = 5
HAPROXY_EXE = "/root/haproxy-1.3.20/haproxy" #EC2
#HAPROXY_EXE = "/scratch/haproxy-1.3.25/haproxy" #Rcluster

class ApacheWebFWScheduler(mesos.Scheduler):
  def __init__(self):
    mesos.Scheduler.__init__(self)
    self.lock = threading.RLock()
    self.id = 0
    self.haproxy = -1
    self.reconfigs = 0
    self.servers = {}
    self.overloaded = False

  def registered(self, driver, fid):
    print "Mesos haproxy+apache scheduler registered as framework #%s" % fid
    self.driver = driver

  def getFrameworkName(self, driver):
      return "haproxy+apache"

  def getExecutorInfo(self, driver):
    execPath = os.path.join(os.getcwd(), "startapache.sh")
    return mesos.ExecutorInfo(execPath, "")

  def reconfigure(self):
    print "reconfiguring haproxy"
    name = "/tmp/haproxy.conf.%d" % self.reconfigs
    with open(name, 'w') as config:
      with open('haproxy.config.template', 'r') as template:
        for line in template:
          config.write(line)
      for id, host in self.servers.iteritems():
        config.write("       ")
        config.write("server %d %s:80 check\n" % (id, host))

    cmd = []
    if self.haproxy != -1:
      cmd = [HAPROXY_EXE,
             "-f",
             name,
             "-sf",
             str(self.haproxy.pid)]
    else:
      cmd = [HAPROXY_EXE,
             "-f",
             name]

    self.haproxy = Popen(cmd, shell = False)
    self.reconfigs += 1

  def resourceOffer(self, driver, oid, slave_offers):
    print "Got resource offer %s with %s slots." % (oid, len(slave_offers))
    self.lock.acquire()
    tasks = []
    for offer in slave_offers:
      if offer.host in self.servers.values():
        print "Rejecting slot on host " + offer.host + " because we've launched a server on that machine already."
        #print "self.servers currently looks like: " + str(self.servers)
      elif not self.overloaded and len(self.servers) > 0:
        print "Rejecting slot because we've launched enough tasks."
      elif int(offer.params['mem']) < 1024:
        print "Rejecting offer because it doesn't contain enough memory (it has " + offer.params['mem'] + " and we need 1024mb."
      elif int(offer.params['cpus']) < 1:
        print "Rejecting offer because it doesn't contain enough CPUs."
      else:
        print "Offer is for " + offer.params['cpus'] + " CPUS and " + offer.params["mem"] + " MB on host " + offer.host
        params = {"cpus": "1", "mem": "1024"}
        td = mesos.TaskDescription(self.id, offer.slaveId, "server %s" % self.id, params, "")
        print "Accepting task, id=" + str(self.id) + ", params: " + params['cpus'] + " CPUS, and " + params['mem'] + " MB, on node " + offer.host
        tasks.append(td)
        self.servers[self.id] = offer.host
        self.id += 1
        self.overloaded = False
    driver.replyToOffer(oid, tasks, {"timeout":"1"})
    #driver.replyToOffer(oid, tasks, {})
    print "done with resourceOffer()"
    self.lock.release()

  def statusUpdate(self, driver, status):
    print "received status update from taskID " + str(status.taskId) + ", with state: " + str(status.state)
    reconfigured = False
    self.lock.acquire()
    if status.taskId in self.servers.keys():
      if status.state == mesos.TASK_STARTING:
        print "Task " + str(status.taskId) + " reported that it is STARTING."
        del self.servers[status.taskId]
        self.reconfigure()
        reconfigured = True
      if status.state == mesos.TASK_RUNNING:
        print "Task " + str(status.taskId) + " reported that it is RUNNING, reconfiguring haproxy to include it in webfarm now."
        self.reconfigure()
        reconfigured = True
      if status.state == mesos.TASK_FINISHED:
        del self.servers[status.taskId]
        print "Task " + str(status.taskId) + " reported FINISHED (state " + status.state + ")."
        self.reconfigure()
        reconfigured = True
      if status.state == mesos.TASK_FAILED:
        print "Task " + str(status.taskId) + " reported that it FAILED!"
        del self.servers[status.taskId]
        self.reconfigure()
        reconfigured = True
      if status.state == mesos.TASK_KILLED:
        print "Task " + str(status.taskId) + " reported that it was KILLED!"
        del self.servers[status.taskId]
        self.reconfigure()
        reconfigured = True
      if status.state == mesos.TASK_LOST:
        print "Task " + str(status.taskId) + " reported was LOST!"
        del self.servers[status.taskId]
        self.reconfigure()
        reconfigured = True
    self.lock.release()
    if reconfigured:
      None#driver.reviveOffers()
    print "done in statusupdate"

  def scaleUp(self):
    print "SCALING UP"
    self.lock.acquire()
    self.overloaded = True
    self.lock.release()

  def scaleDown(self, id):
    print "SCALING DOWN (removing server %d)" % id
    kill = False
    self.lock.acquire()
    if self.overloaded:
      self.overloaded = False
    else:
      kill = True
    self.lock.release()
    if kill:
      self.driver.killTask(id)


def monitor(sched):
  print "in MONITOR()"
  while True:
    time.sleep(1)
    print "done sleeping"
    try:
      conn = httplib.HTTPConnection("r3.millennium.berkeley.edu:9001")
      print "done creating connection"
      conn.request("GET", "/stats;csv")
      print "done with request()"
      res = conn.getresponse()
      print "testing response status"
      if (res.status != 200):
        print "response != 200"
        continue
      else:
        print "got some stats"
        data = res.read()
        lines = data.split('\n')[2:-2]

        data = data.split('\n')
        data = data[1].split(',')

        if int(data[33]) >= START_THRESHOLD:
          sched.scaleUp()
        elif int(data[4]) <= KILL_THRESHOLD:
          minload, minid = (sys.maxint, 0)
          for l in lines:
            cols = l.split(',')
            id = int(cols[1])
            load = int(cols[4])
            if load < minload:
              minload = load
              minid = id

          if len(lines) > MIN_SERVERS and minload == 0:
            sched.scaleDown(minid)

        conn.close()
    except Exception, e:
      print "exception in monitor()"
      continue
  print "done in MONITOR()"

if __name__ == "__main__":
  parser = OptionParser(usage = "Usage: %prog mesos_master")

  (options,args) = parser.parse_args()
  if len(args) < 1:
    print >> sys.stderr, "At least one parameter required."
    print >> sys.stderr, "Use --help to show usage."
    exit(2)

  print "sched = ApacheWebFWScheduler()"
  sched = ApacheWebFWScheduler()

  print "Connecting to mesos master %s" % args[0]
  driver = mesos.MesosSchedulerDriver(sched, sys.argv[1])

  threading.Thread(target = monitor, args=[sched]).start()

  driver.run()

  print "Scheduler finished!"
