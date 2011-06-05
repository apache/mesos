#!/usr/bin/env python
import nexus
import sys
import time
import os
import atexit

from subprocess import *

PBS_MOM_EXE = "/usr/local/sbin/pbs_mom"
MOMCTL_EXE = "/usr/local/sbin/momctl"
TORQUE_CFG = "/var/spool/torque"
PBS_MOM_CONF_FILE = TORQUE_CFG + "/mom_priv/config"

def cleanup():
  try:
    # TODO(*): This will kill ALL mpds...oops.
    print "cleanup"
    os.waitpid(Popen("momctl -s", shell=True).pid, 0)
  except Exception, e:
    print e
    None

class MyExecutor(nexus.Executor):
  def __init__(self):
    nexus.Executor.__init__(self)

  def init(self, arg):
    print "in torque executor init"
    self.pbs_server_ip = arg.data

  def startTask(self, task):
    print "Running task %d" % task.taskId
    
    #TODO: check to see if pbs_mom is installed, if not install it
    #   simply requires getting and running the installer that gets
    #   built when you make torque, like torque-package-mom-linux-ia64.sh
    #   or maybe apt-get install pbs-mom

    #if Popen("which pbs_mom", shell=True).wait() != 0:
    #print "pbs_mom command not found, tring to apt-get install torque-mom"
    #try:
    #  print Popen("apt-get -y install torque-mom",shell=True).wait()
    #except:
    #  print "error installing pbs_mom, please install it on compute nodes"
    #  None

    print "checking pbs_mom conf file " + PBS_MOM_CONF_FILE + " is it a file? "\
           + str(os.path.isfile(PBS_MOM_CONF_FILE))
    #TODO: check to see if file exists, if it does, check to see that it is correct
    #      else create it (right now we overwrite it no matter what)
    if not os.path.isfile(PBS_MOM_CONF_FILE):
      print PBS_MOM_CONF_FILE + " file not found, about to create it"
    else:
      print "about to overwrite file " + PBS_MOM_CONF_FILE + " to update "\
            "pbs_server on this node"

    print "adding line to conf file: $pbsserver " + self.pbs_server_ip + "\n"
    FILE = open(PBS_MOM_CONF_FILE,'w')
    FILE.write("$pbsserver " + self.pbs_server_ip + "\n")
    FILE.write("$logevent 255 #bitmap of which events to log\n")

    FILE.close()
   
    print "overwrote pbs_mom config file, its contents now are:"
    FILE = open(PBS_MOM_CONF_FILE,'r')
    for line in FILE: print line + "\n"
    FILE.close()

    #try killing pbs_mom in case we changed the config
    if Popen("momctl -s",shell=True).wait() != 0:
      print "tried to kill pbs_mom, but it was not running"

    #run pbs_mom
    print "running pbs_mom on compute node"
    Popen("pbs_mom", shell=True)

  def killTask(self, tid):
    sys.exit(1)

  def shutdown(self):
    print "shutdown"
    cleanup()

  def error(self, code, message):
    print "Error: %s" % message

if __name__ == "__main__":
  print "Starting pbs_mom executor"
  atexit.register(cleanup)
  executor = MyExecutor()
  executor.run()
