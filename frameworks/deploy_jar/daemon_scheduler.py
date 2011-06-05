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
#HAPROXY_EXE = "/root/haproxy-1.3.20/haproxy"
HAPROXY_EXE = "/home/andyk/mesos/frameworks/haproxy+apache/haproxy-1.3.20/haproxy"

class MyScheduler(mesos.Scheduler):
  def __init__(self, num_tasks, jar_url, jar_class, jar_args):
    mesos.Scheduler.__init__(self)
    self.lock = threading.RLock()
    self.id = 0
    self.haproxy = -1
    self.reconfigs = 0
    self.task_count = 0
    self.overloaded = False
    self.num_tasks = num_tasks
    self.jar_url = jar_url
    self.jar_class = jar_class
    self.jar_args = jar_args

#  def reconfigure(self):
#    name = "/tmp/haproxy.conf.%d" % self.reconfigs
#    with open(name, 'w') as config:
#      with open('haproxy.config.template', 'r') as template:
#        for line in template:
#          config.write(line)
#      for id, host in self.servers.iteritems():
#        config.write("       ")
#        config.write("server %d %s:80 check\n" % (id, host))
#
#    cmd = []
#    if self.haproxy != -1:
#      cmd = [HAPROXY_EXE,
#             "-f",
#             name,
#             "-sf",
#             str(self.haproxy.pid)]
#    else:
#      cmd = [HAPROXY_EXE,
#             "-f",
#             name]
#
#    self.haproxy = Popen(cmd, shell = False)
#    self.reconfigs += 1
  
  def getExecutorInfo(self, driver):
    execPath = os.path.join(os.getcwd(), "daemon_executor.sh")
    return mesos.ExecutorInfo(execPath, "")

  def registered(self, driver, fid):
    print "Mesos daemon scheduler registered as framework #%s" % fid

  def resourceOffer(self, driver, oid, slave_offers):
    print "Got slot offer %d" % oid
    self.lock.acquire()
    tasks = []
    for offer in slave_offers:
      if int(self.task_count) < int(self.num_tasks) and int(offer.params['mem']) >= 1073741824 and int(offer.params['cpus']) > 0:
        print "accept slot here"
        params = {"cpus": "1", "mem": "1073741824"}
        task_args = self.jar_url + "\t" + self.jar_class + "\t" + self.jar_args
        print "task args are: " + task_args
        td = mesos.TaskDescription(
            self.task_count, offer.slaveId, "task %d" % self.task_count, params, task_args)
        tasks.append(td)
        print "incrementing self.task_count from " + str(self.task_count)
        print "self.num_tasks is " + str(self.num_tasks)
        self.task_count += 1
      else:
        print "Rejecting slot because we've launched enough tasks"
    driver.replyToOffer(oid, tasks, {"timeout": "1"})
    #self.reconfigure()
    self.lock.release()

  #def statusUpdate(self, driver, status):
  #  self.lock.acquire()
  #  if status.taskId in self.servers.keys():
  #    if status.state == mesos.TASK_FINISHED:
  #      del self.servers[status.taskId]
  #      self.reconfigure()
  #      reconfigured = True
  #  self.lock.release()
  #  if reconfigured:
  #    self.reviveOffers()

  #def scaleUp(self):
  #  print "SCALING UP"
  #  self.lock.acquire()
  #  self.overloaded = True
  #  self.lock.release()

  #def scaleDown(self, id):
  #  print "SCALING DOWN (removing server %d)" % id
  #  kill = False
  #  self.lock.acquire()
  #  if self.overloaded:
  #    self.overloaded = False
  #  else:
  #    kill = True
  #  self.lock.release()
  #  if kill:
  #    self.killTask(id)


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
  #parser = OptionParser(usage = "Usage: daemon_framework <mesos-master> <num tasks> <URL of jar> <class name> <arguments>")

  #(options,args) = parser.parse_args()
  #if len(args) < 5:
  #  print >> sys.stderr, "five parameters required. " + str(len(args))
  #  print >> sys.stderr, "Use --help to show usage."
  #  exit(2)
  args = sys.argv[1:len(sys.argv)]
  
  print "sched = MyScheduler(" + args[1] + ", "+  args[2]+ ", "+ args[3]+ ", "+ " ".join(args[4:len(args)])+")"

  sched = MyScheduler(args[1], args[2], args[3], " ".join(args[4:len(args)]))

  #threading.Thread(target = monitor, args=[sched]).start()

  print "Connecting to mesos master %s" % args[0]

  mesos.MesosSchedulerDriver(sched, sys.argv[1]).run()

  print "Finished!"
