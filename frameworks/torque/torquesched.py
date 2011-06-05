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
    self.id = 0
    self.ip = ip 
    self.servers = {}
    self.overloaded = False
  

  def getExecutorInfo(self, driver):
    execPath = os.path.join(os.getcwd(), "start_pbs_mom.sh")
    initArg = ip # tell executor which node the pbs_server is running on
    print "in getExecutorInfo, setting execPath = " + execPath + " and initArg = " + initArg
    return nexus.ExecutorInfo(execPath, initArg)

  def registered(self, driver, fid):
    print "Nexus torque+pbs scheduler registered as framework #%s" % fid

   
  def resourceOffer(self, driver, oid, slave_offers):
    print "Got slot offer %d" % oid
    tasks = []
    for offer in slave_offers:
      # if we haven't registered this node, accept slot & register w pbs_server#      #TODO: check to see if slot is big enough 
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
      regComputeNode(offer.host)
      self.id += 1
      print "self.id now set to " + str(self.id)
    driver.replyToOffer(oid, tasks, {"timeout": "-1"})

#  def statusUpdate(self, status):
#    if status.taskId in self.servers.keys():
#      if status.state == nexus.TASK_FINISHED:
#        del self.servers[status.taskId]
#        reconfigured = True
#    if reconfigured:
#      self.reviveOffers()

def regComputeNode(new_node):
  print "registering new compute node, "+new_node+", with pbs_server"

  print "checking to see if node is registered with server already"
  nodes = Popen("pbsnodes", shell=True, stdout=PIPE).stdout
  print "output of pbsnodes command is: "
  for line in nodes: 
    print line
    if line.find(new_node) != -1:
      print "Warn: tried to register a node that is already registered, skipping"
      return

  #add node to server
  print "registering node w/ pbs_server using: qmgr -c create node " + new_node
  qmgr_add = Popen("qmgr -c \"create node " + new_node + "\"", shell=True, stdout=PIPE).stdout
  print "output of qmgr:"
  for line in qmgr_add: print line

def unregComputeNode(node_name):
  #remove node from server
  print("removing node from pbs_server using: qmgr -c delete node " + node)
  print Popen(QMGR_EXE + ' "-c delete node ' + node + '"').stdout

def unregAllNodes():
  for node in self.servers:
    print "unregistering node " + node
    unregComputeNode(node)

def getFrameworkName(self, driver):
  return "Nexus torque Framework"

def monitor(sched):
  while True:
    time.sleep(1)
    try:
      print "checking to see if queue is empty"
      qstat = Popen("qstat -Q",shell=True,stdout=PIPE).stdout
      for line in qstat:
         #print "line is " + line
         if re.match('^batch.*', line):
           #print "found header line, next line has data"
           jobcount = int(line.split()[5]) + int(line.split()[6]) + int(line.split()[7]) + int(line.split()[8])
           if jobcount == 0:
             print "no incomplete jobs remaining in queue, releasing all slots"
             unregAllNodes()
    except Exception, e:
      print "error running/parsing qstat"
      print e
      continue

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
  Popen("/etc/init.d/pbs_server start", shell=True)

  print "starting pbs_scheduler"
  Popen("/etc/init.d/pbs_sched start", shell=True)

  ip = Popen("hostname -i", shell=True, stdout=PIPE).stdout.readline().rstrip()
  print "Remembering IP address of scheduler (" + ip + ")"

  print "Connecting to nexus master %s" % args[0]

  sched = MyScheduler(ip)
  threading.Thread(target = monitor, args=[sched]).start()

  nexus.NexusSchedulerDriver(sched, args[0]).run()

  print "Finished!"
