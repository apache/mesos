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
import torquelib

from optparse import OptionParser
from subprocess import *
from socket import gethostname

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
    self.driver = driver
    print "Got slot offer %d" % oid
    self.lock.acquire()
    print "resourceOffer() acquired lock"
    tasks = []
    for offer in slave_offers:
      # if we haven't registered this node, accept slot & register w pbs_server
      #TODO: check to see if slot is big enough 
      if self.numToRegister <= 0:
        print "Rejecting slot, no need for more slaves"
        continue
      if offer.host in self.servers.values():
        print "Rejecting slot, already registered node " + offer.host
        continue
      if len(self.servers) >= SAFE_ALLOCATION["cpus"]:
        print "Rejecting slot, already at safe allocation (i.e. %d CPUS)" % SAFE_ALLOCATION["cpus"]
        continue
      print "Need %d more nodes, so accepting slot, setting up params for it..." % self.numToRegister
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
    print "got status update from TID %s, state is: %s, data is: %s" %(status.taskId,status.state,status.data)

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
  
  #unreg up to N random compute nodes, leave at least one
  def unregNNodes(self, num_nodes):
    print "unregNNodes called with arg %d" % num_nodes
    if num_nodes > len(self.servers)-1:
      print "... however, only unregistering %d nodes, leaving one alive" % (len(self.servers)-1)
    print "getting and filtering list of nodes using torquelib"
    noJobs = lambda x: x.status.has_key("jobs") == False or (x.status.has_key("jobs") == True and x.status["jobs"] == "")
    inactiveNodes = map(lambda x: x.name,filter(noJobs, torquelib.getNodes()))
    print "victim pool of inactive nodes:"
    for inode in inactiveNodes:
      print inode
    for tid, hostname in self.servers.items():
      if len(self.servers) > 1 and num_nodes > 0 and hostname in inactiveNodes:
        print "we still have to kill %d of the %d compute nodes which master is tracking" % (num_nodes, len(self.servers))
        print "unregistering node " + str(hostname)
        self.unregComputeNode(hostname)
        self.servers.pop(tid)
        num_nodes = num_nodes - 1
        print "killing corresponding task with tid %d" % tid
        self.driver.killTask(tid)
    #handle the case where we didn't kill enough nodes 

  def getFrameworkName(self, driver):
    return "Nexus Torque Framework"


def monitor(sched):
  while True:
    time.sleep(1)
    print "monitor thread acquiring lock"
    sched.lock.acquire()
    print "computing num nodes needed to satisfy eligable jobs in queue"
    needed = 0
    jobs = torquelib.getActiveJobs()
    print "retreived jobs in queue, count: %d" % len(jobs)
    for j in jobs:
      #WARNING: this check should only be used if torque is using fifo queue
      #if needed + j.needsnodes <= SAFE_ALLOCATION:
      print "job resource list is: " + str(j.resourceList)
      needed += int(j.resourceList["nodect"])
    print "number of nodes needed by jobs in queue: %d" % needed
    numToRelease = len(sched.servers) - needed
    print "number of nodes to release is %d - %d" % (len(sched.servers),needed)
    if numToRelease > 0:
      sched.unregNNodes(numToRelease)
      sched.numToRegister = 0
    else:
      print "monitor updating sched.numToRegister from %d to %d" % (sched.numToRegister, numToRelease * -1)
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
  time.sleep(1)

  print "writing $(TORQUECFG)/server_name file with fqdn of pbs_server: " + fqdn
  FILE = open(PBS_SERVER_FILE,'w')
  FILE.write(fqdn)
  FILE.close()

  print "starting pbs_server"
  #Popen("/etc/init.d/pbs_server start", shell=True)
  Popen("pbs_server", shell=True)
  time.sleep(2)
  print "\n"

  print "running command: qmgr -c \"set queue batch resources_available.nodes=%s\"" % SAFE_ALLOCATION["cpus"]
  Popen("qmgr -c \"set queue batch resources_available.nodect=%s\"" % SAFE_ALLOCATION["cpus"], shell=True)
  Popen("qmgr -c \"set server resources_available.nodect=%s\"" % SAFE_ALLOCATION["cpus"], shell=True)

  #these lines might not be necessary since we hacked the torque fifo scheduler
  Popen("qmgr -c \"set queue batch resources_max.nodect=%s\"" % SAFE_ALLOCATION["cpus"], shell=True)
  Popen("qmgr -c \"set server resources_max.nodect=%s\"" % SAFE_ALLOCATION["cpus"], shell=True)

  outp = Popen("qmgr -c \"l queue batch\"", shell=True, stdout=PIPE).stdout
  for l in outp:
    print l

  print "RE-killing pbs_server for resources_available setting to take effect"
  #Popen("/etc/init.d/pbs_server start", shell=True)
  Popen("qterm", shell=True)
  #time.sleep(1)
  print("\n")

  print "RE-starting pbs_server for resources_available setting to take effect"
  Popen("pbs_server", shell=True)
  "qmgr list queue settings: "
  output = Popen("qmgr -c 'l q batch'", shell=True, stdout=PIPE).stdout
  for line in output:
    print line
  print("\n")

  print "running killall pbs_sched"
  Popen("killall pbs_sched", shell=True)
  #time.sleep(2)
  print("\n")

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
