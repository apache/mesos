#!/usr/bin/env python

import nexus
import os
import sys
import time
import httplib
import Queue
import threading
import re
import socket
import qstat

from optparse import OptionParser
from subprocess import *
from socket import gethostname
#from qstat import *

PBS_SERVER_FILE = "/var/spool/torque/server_name"

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
    self.numToRegister = 1
  
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
      # if we haven't registered this node, accept slot & register w pbs_server
      #TODO: check to see if slot is big enough 
      if self.numToRegister <= 0:
        print "Rejecting slot, job queue is empty (only keep one slave around)"
        continue
      if offer.host in self.servers.values():
        print "Rejecting slot, already registered node " + offer.host
        continue
      if len(self.servers) >= SAFE_ALLOCATION["cpus"]:
        print "Rejecting slot, already at safe allocation (i.e. %d CPUS)" % SAFE_ALLOCATION["cpus"]
        continue
      print "Accepting slot, setting up params for it..."
      params = {"cpus": "1", "mem": "1073741824"}
      td = nexus.TaskDescription(
          self.id, offer.slaveId, "task %d" % self.id, params, "")
      tasks.append(td)
      self.servers[self.id] = offer.host
      self.regComputeNode(offer.host)
      self.numToRegister -= 1
      self.id += 1
      print "self.id now set to " + str(self.id)
    print "---"
    driver.replyToOffer(oid, tasks, {"timeout": "1"})
    self.lock.release()
    print "resourceOffer() finished, released lock"
    print "\n"

  def statusUpdate(self, driver, status):
    print "got status update, data is: " + status.data

  def regComputeNode(self, new_node):
    print "registering new compute node, "+new_node+", with pbs_server"
    print "checking to see if node is registered with server already"
    #nodes = Popen("qmgr -c 'list node'", shell=True, stdout=PIPE).stdout
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
  
  #leave one node so that the queue can accept new jobs
  def unregAllNodes(self):
    for key, val in self.servers.items():
      if (self.servers) > 1:
        print "unregistering node " + str(val)
        self.unregComputeNode(val)
        self.servers.pop(key)
  
  #unreg up to N random compute nodes, leave at least one
  def unregNNodes(self, num_nodes):
    print "unregistering %d nodes" % num_nodes
    for key, val in self.servers.items():
      if (self.servers) > 1 and num_nodes > 0:
        print "unregistering node " + str(val)
        self.unregComputeNode(val)
        self.servers.pop(key)
        num_nodes = num_nodes - 1

  def getFrameworkName(self, driver):
    return "Nexus Torque Framework"


def monitor(sched):
  while True:
    time.sleep(1)
    print "monitor thread acquiring lock"
    sched.lock.acquire()
    if sched.queueLength() == 0:
      #print "no incomplete jobs in queue, attempting to release all slots"
      if len(sched.servers) == 0:
        #print "no servers registered, so no need to call unregAllNodes()"
        print "monitor thread releasing lock"
        print "\n"
        sched.lock.release()
        continue
      print "unregistering all nodes because no jobs running"
      sched.unregAllNodes()
    else:
      #adjust num servers registered to match num needed 
      #by frist N jobs in #the queue. 
      print "computing num nodes needed to satisfy first N jobs in queue"
      needed = 0
      jobs = qstat.getActiveJobs()
      print "retreived jobs in queue, count: %d" % len(jobs)
      for j in jobs:
        #WARNING: this check should only be used if torque is using fifo queue
        #if needed + j.needsnodes <= SAFE_ALLOCATION:
        needed += j.resourceList["neednodes"]
      print "number of nodes needed by jobs in queue: %d" % needed
      numToRelease = len(self.servers) - needed
      if numToRelease > 0:
        sched.unregNNodes(numToRelease)
        sched.numToRegister = 0
      else:
        print "monitor updating sched.numToRelease from %d to %d" % (sched.numToRegister, numToRelease * -1)
        sched.numToRegister = numToRelease * -1
    sched.lock.release()
    print "monitor thread releasing lock"
    print "\n"

if __name__ == "__main__":
  parser = OptionParser(usage = "Usage: %prog nexus_master")

  (options,args) = parser.parse_args()
  if len(args) < 1:
    print >> sys.stderr, "At least one parameter required."
    print >> sys.stderr, "Use --help to show usage."
    exit(2)

  fqdn = socket.getfqdn()
  ip = socket.gethostbyname(gethostname())

  print "running killall pbs_server"
  Popen("killall pbs_server", shell=True)

  print "writing $(TORQUECFG)/server_name file with fqdn of pbs_server: " + fqdn
  FILE = open(PBS_SERVER_FILE,'w')
  FILE.write(fqdn)
  FILE.close()

  time.sleep(2)
  print "starting pbs_server"
  #Popen("/etc/init.d/pbs_server start", shell=True)
  Popen("pbs_server", shell=True)

  print "running killall pbs_sched"
  Popen("killall pbs_sched", shell=True)
  
  time.sleep(2)
  print "starting pbs_scheduler"
  #Popen("/etc/init.d/pbs_sched start", shell=True)
  Popen("pbs_sched", shell=True)

  #ip = Popen("hostname -i", shell=True, stdout=PIPE).stdout.readline().rstrip() #linux
  #ip = Popen("ifconfig en1 | awk '/inet / { print $2 }'", shell=True, stdout=PIPE).stdout.readline().rstrip() # os x
  print "Remembering IP address of scheduler (" + ip + "), and fqdn: " + fqdn

  print "Connecting to nexus master %s" % args[0]

  sched = MyScheduler(fqdn)
  threading.Thread(target = monitor, args=[sched]).start()

  nexus.NexusSchedulerDriver(sched, args[0]).run()

  print "Finished!"
