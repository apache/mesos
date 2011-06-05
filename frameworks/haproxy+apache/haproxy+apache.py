#!/usr/bin/env python

import nexus
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
#HAPROXY_EXE = "/root/haproxy-1.3.20/haproxy"
HAPROXY_EXE = "/home/andyk/nexus/frameworks/haproxy+apache/haproxy-1.3.20/haproxy"

class Scheduler(nexus.Scheduler):
  def __init__(self):
    nexus.Scheduler.__init__(self, "haproxy+apache",
        os.path.join(os.getcwd(), "startapache.sh"), "")
    self.lock = threading.RLock()
    self.id = 0
    self.haproxy = -1
    self.reconfigs = 0
    self.servers = {}
    self.overloaded = False

  def reconfigure(self):
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

  def registered(self, fid):
    print "Nexus haproxy+apache scheduler registered as framework #%s" % fid

  def slotOffer(self, oid, slots):
    print "Got slot offer %d" % oid
    self.lock.acquire()
    tasks = []
    for slot in slots:
      if not slot.host in self.servers.values() and self.overloaded or len(self.servers) < 1:
        params = "cpus=1\nmem=1073741824"
        td = nexus.TaskDescription(self.id, slot.slaveId, "server %s" % self.id, params, "")
        tasks.append(td)
        self.servers[self.id] = slot.host
        self.id += 1
      else:
        print "Rejecting slot because we've launched enough tasks"
      self.replyToOffer(oid, tasks, "timeout=1")
    self.reconfigure()
    self.lock.release()

  def statusUpdate(self, status):
    reconfigured = False
    self.lock.acquire()
    if status.taskId in self.servers.keys():
      if status.state == nexus.TASK_FINISHED:
        del self.servers[status.taskId]
        self.reconfigure()
        reconfigured = True
    self.lock.release()
    if reconfigured:
      self.reviveOffers()

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
      self.killTask(id)


def monitor(sched):
  while True:
    time.sleep(1)
    try:
      conn = httplib.HTTPConnection("ec2-72-44-51-87.compute-1.amazonaws.com")
      conn.request("GET", "/stats;csv")
      res = conn.getresponse()
      if (res.status != 200):
        print "response != 200"
        continue
      else:
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
      continue

if __name__ == "__main__":
  parser = OptionParser(usage = "Usage: %prog nexus_master")

  (options,args) = parser.parse_args()
  if len(args) < 1:
    print >> sys.stderr, "At least one parameter required."
    print >> sys.stderr, "Use --help to show usage."
    exit(2)

  sched = Scheduler()

  threading.Thread(target = monitor, args=[sched]).start()

  print "Connecting to nexus master %s" % args[0]

  sched.run(args[0])

  print "Finished!"
