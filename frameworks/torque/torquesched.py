#!/usr/bin/env python

import nexus
import os
import sys
import time
import httplib
import Queue
import threading
import re

from optparse import OptionParser
from subprocess import *
from socket import gethostname

SAFE_ALLOCATION = {"cpus":5,"mem":134217728} #just set statically for now, 128MB
MIN_SLOT_SIZE = {"cpus":"1","mem":1073741824} #1GB

class MyScheduler(nexus.Scheduler):
  def __init__(self, ip):
    nexus.Scheduler.__init__(self)
    self.lock = threading.RLock()
    self.id = 0
    self.ip = ip 
    self.servers = {}
    self.overloaded = False
  
  def getExecutorInfo(self, driver):
    execPath = os.path.join(os.getcwd(), "start_pbs_mom.sh")
    initArg = self.ip # tell executor which node the pbs_server is running on
    print "in getExecutorInfo, setting execPath = " + execPath + " and initArg = " + initArg
    return nexus.ExecutorInfo(execPath, initArg)

  def registered(self, driver, fid):
    print "Nexus torque+pbs scheduler registered as framework #%s" % fid

  def resourceOffer(self, driver, oid, slave_offers):
    print "Got slot offer %d" % oid
    self.lock.acquire()
    print "resourceOffer() acquired lock"
    tasks = []
    for offer in slave_offers:
      # if we haven't registered this node, accept slot & register w pbs_server#      #TODO: check to see if slot is big enough 
      if self.queueLength() == 0:
        print "Rejecting slot, job queue is empty"
        continue
      if offer.host in self.servers.values():
        print "Rejecting slot, already registered node " + offer.host
        continue
      if len(self.servers) >= SAFE_ALLOCATION:
        print "Rejecting slot, already at safe allocation"
        continue
      print "Accepting slot, setting up params for it..."
      params = {"cpus": "%d" % 1, "mem": "%d" % 1073741824}
      td = nexus.TaskDescription(
          self.id, offer.slaveId, "task %d" % self.id, params, "")
      tasks.append(td)
      self.servers[self.id] = offer.host
      self.regComputeNode(offer.host)
      self.id += 1
      print "self.id now set to " + str(self.id)
    print ""
    driver.replyToOffer(oid, tasks, {"timeout": "1"})
    self.lock.release()

  def regComputeNode(self, new_node):
    print "registering new compute node, "+new_node+", with pbs_server"
    print "checking to see if node is registered with server already"
    nodes = Popen("pbsnodes", shell=True, stdout=PIPE).stdout
    print "output of pbsnodes command is: "
    for line in nodes: 
      print line
      if line.find(new_node) != -1:
        print "Warn: tried to register node that's already registered, skipping"
        return
    #add node to server
    print "registering node with command: qmgr -c create node " + new_node
    qmgr_add = Popen("qmgr -c \"create node " + new_node + "\"", shell=True, stdout=PIPE).stdout
    print "output of qmgr:"
    for line in qmgr_add: print line

  def unregComputeNode(self, node_name):
    #remove node from server
    print("removing node from pbs_server: qmgr -c delete node " + node_name)
    print Popen('qmgr -c "delete node ' + node_name + '"', shell=True, stdout=PIPE).stdout
  
  def unregAllNodes(self):
    for node in self.servers.values():
      print "unregistering node " + str(node)
      self.unregComputeNode(node)
      self.servers.pop(node)
  
  def getFrameworkName(self, driver):
    return "Nexus torque Framework"
  
  def queueLength(self):
    print "computing the number of active jobs in the queue"
    qstat = Popen("qstat -Q",shell=True,stdout=PIPE).stdout
    jobcount = 0
    for line in qstat:
       if re.match('^batch.*', line):
         jobcount = int(line.split()[5]) + int(line.split()[6]) + int(line.split()[7]) + int(line.split()[8])
    return jobcount

def monitor(sched):
  while True:
    time.sleep(1)
    print "monitor thread acquiring lock"
    sched.lock.acquire()
    if sched.queueLength() == 0:
      print "no incomplete jobs in queue, attempting to release all slots"
      if len(sched.servers) == 0:
        print "no servers registered, so no need to call unregAllNodes()"
        print "monitor thread releasing lock"
        sched.lock.release()
        continue
      sched.unregAllNodes()
    print ""
    sched.lock.release()
    print "monitor thread releasing lock"

if __name__ == "__main__":
  parser = OptionParser(usage = "Usage: %prog nexus_master")

  (options,args) = parser.parse_args()
  if len(args) < 1:
    print >> sys.stderr, "At least one parameter required."
    print >> sys.stderr, "Use --help to show usage."
    exit(2)

  print "running qterm"
  Popen("qterm", shell=True).wait()

  print "starting pbs_server"
  #Popen("/etc/init.d/pbs_server start", shell=True)
  Popen("pbs_server", shell=True)

  print "starting pbs_scheduler"
  #Popen("/etc/init.d/pbs_sched start", shell=True)
  Popen("pbs_sched", shell=True)

  #ip = Popen("hostname -i", shell=True, stdout=PIPE).stdout.readline().rstrip() #linux
  ip = Popen("ifconfig en1 | awk '/inet / { print $2 }'", shell=True, stdout=PIPE).stdout.readline().rstrip() # os x
  print "Remembering IP address of scheduler (" + ip + "), type: " + str(type(ip))

  print "Connecting to nexus master %s" % args[0]

  sched = MyScheduler(ip)
  threading.Thread(target = monitor, args=[sched]).start()

  nexus.NexusSchedulerDriver(sched, args[0]).run()

  print "Finished!"
