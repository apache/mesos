#!/usr/bin/env python

import nexus
import os
import sys
import time
import httplib
import Queue

from optparse import OptionParser
from subprocess import *
from socket import gethostname

SAFE_ALLOCATION = {"cpus":5,"mem":134217728} #just set statically for now, 128MB
MIN_SLOT_SIZE = {"cpus":"1","mem":1073741824} #1GB

TORQUE_DL_URL = "http://www.clusterresources.com/downloads/torque/torque-2.4.6.tar.gz"
TORQUE_UNCOMPRESSED_DIR = "torque-2.4.6"

TORQUE_CFG = "/var/spool/torque"
PBS_SERVER_CONF_FILE = TORQUE_CFG + "/server_priv/nodes" #hopefully can use qmgr and not edit this

TORQUE_INSTALL_DIR = "/usr/local/sbin"
PBS_SERVER_EXE = TORQUE_INSTALL_DIR + "/pbs_server"
SCHEDULER_EXE = TORQUE_INSTALL_DIR + "/pbs_sched"
QMGR_EXE = "/usr/local/bin/qmgr"


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

   
  #DESIGN PLAN:
  #
  #for each slot in the offer
  #  if we are at safe allocation, don't accept any more of these slot offers
  #  else if the slot is not >= min-slot size, reject
  #  else if we have already set up some resources on this machine and started
  #          pbs_mom on it, don't accept more resources.
  #            TODO: Eventually, set up max and min resources per compute node and
  #            accept resources on an existing compute node and just update
  #            the config settings for the torque daemon on that node to use
  #            more resources on it.
  #  else accept the offer and launch pbs_mom on the node
  def resourceOffer(self, driver, oid, slave_offers):
    print "Got slot offer %d" % oid
    tasks = []
    for offer in slave_offers:
      # for a first step, just keep track of whether we have started a pbs_mom
      # on this node before or not, if not then accept the slot and launch one
      if not offer.host in self.servers.values(): 
        #TODO: check to see if slot is big enough 
        print "setting up params"
        #print "params = cpus: "# + MIN_SLOT_SIZE["cpus"]# +  ", mem: " + MIN_SLOT_SIZE["mem"]
        params = {"cpus": "%d" % 1, "mem": "%d" % 1073741824}
        td = nexus.TaskDescription(
            self.id, offer.slaveId, "task %d" % self.id, params, "")
        tasks.append(td)
        self.servers[self.id] = offer.host
        regComputeNode(offer.host)
        self.id += 1
        print "self.id now set to " + str(self.id)
      else:
        print "Rejecting slot because we've aleady launched pbs_mom on that node"
    driver.replyToOffer(oid, tasks, {"timeout": "-1"})

#  def statusUpdate(self, status):
#    if status.taskId in self.servers.keys():
#      if status.state == nexus.TASK_FINISHED:
#        del self.servers[status.taskId]
#        reconfigured = True
#    if reconfigured:
#      self.reviveOffers()

def regComputeNode(new_node):
  print "in reg"
  print "chdir"
  os.chdir(TORQUE_CFG)
  # first grep the server conf file to be sure this node isn't already
  # registered with the server
  f = open(PBS_SERVER_CONF_FILE, 'r')
  for line in f:
    if line.find(new_node) != -1:
      print "ERROR! Trying to register compute node which "\
            "is already registered, aborting node register"
      return
  f.close()

  #add node to server
  print("adding a node to pbs_server using: qmgr -c create node " + new_node)
  Popen(QMGR_EXE + ' "-c create node ' + new_node + '"',shell=True, stdout=PIPE).stdout

#    #add line to server_priv/nodes file for new_node
#    f = open(PBS_SERVER_CONF_FILE,'a')
#    f.write(new_node+"\n")
#    f.close

def unregComputeNode(node_name):
  #remove node from server
  print("removing a node from pbs_server using: qmgr -c delete node " + node)
  print Popen(QMGR_EXE + ' "-c delete node ' + node + '"').stdout
def getFrameworkName(self, driver):
  return "Nexus torque Framework"

if __name__ == "__main__":
  parser = OptionParser(usage = "Usage: %prog nexus_master")

  (options,args) = parser.parse_args()
  if len(args) < 1:
    print >> sys.stderr, "At least one parameter required."
    print >> sys.stderr, "Use --help to show usage."
    exit(2)

  print "setting up pbs_server"
 
  #if torque isn't installed, install our own
  #print "Torque doesn't seem to be installed. Downloading and installing it"
  #Popen("curl " + TORQUE_DL_URL + " > torque.tgz)
  #Popen("tar xzf torque.tgz")
  #os.chdir(TORQUE_UNCOMPRESSED_DIR)
  #Popen(os.getcwd()+"/configure --prefix=/usr/local")
  #Popen("make")
  #Popen("make install")
  #Popen(os.getcwd()+"torque.setup root")
  
  try:
    pbs_server = Popen(PBS_SERVER_EXE)
  except OSError,e:
    print >>sys.stderr, "Error starting pbs server"
    print >>sys.stderr, e
    exit(2)

  try:
    pbs_scheduler = Popen(SCHEDULER_EXE)
  except OSError,e:
    print >>sys.stderr, "Error starting scheduler"
    print >>sys.stderr, e
    exit(2)

  ip = Popen("hostname -i", shell=True, stdout=PIPE).stdout.readline().rstrip()
  print "Remembering IP address of scheduler (" + ip + ")"

  print "Connecting to nexus master %s" % args[0]

  sched = MyScheduler(ip)
  nexus.NexusSchedulerDriver(sched, args[0]).run()

  #WARNING: this is new in python 2.6
  pbs_server.terminate() #don't leave pbs_server running, not sure if this works

  print "Finished!"
