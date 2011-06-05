#!/usr/bin/env python
import mesos
import sys
import time
import os
import atexit

from subprocess import *

PBS_MOM_CONF_FILE = "/var/spool/torque/mom_priv/config"
PBS_SERVER_NAME_FILE = "/var/spool/torque/server_name"

def cleanup():
  try:
    # TODO(*): This will kill ALL mpds...oops.
    print "cleanup"
    os.waitpid(Popen("momctl -s", shell=True).pid, 0)
  except Exception, e:
    print e
    None

class MyExecutor(mesos.Executor):
  def __init__(self):
    mesos.Executor.__init__(self)

  def init(self, driver, arg):
    print "in torque executor init"
    print "initializing self.pbs_server_fqdn to " + str(arg.data)
    self.pbs_server_fqdn = arg.data

  def launchTask(self, driver, task):
    print "Running task %d" % task.taskId
    
    #TODO: if config file exists, check to see that it is correct
    #      (right now we overwrite it no matter what)

    #print "checking pbs_mom conf file " + PBS_MOM_CONF_FILE + " is it a file? "\
    #       + str(os.path.isfile(PBS_MOM_CONF_FILE))
    #if not os.path.isfile(PBS_MOM_CONF_FILE):
    #  print PBS_MOM_CONF_FILE + " file not found, about to create it"
    #else:
    #  print "about to overwrite file " + PBS_MOM_CONF_FILE + " to update "#\
    #         + "pbs_server on this node"

    #print "adding line to conf file: $pbsserver " + self.pbs_server_ip + "\n"
    #FILE = open(PBS_MOM_CONF_FILE,'w')
    #FILE.write("$pbsserver " + self.pbs_server_ip + "\n")
    #FILE.write("$logevent 255 #bitmap of which events to log\n")
    #FILE.close()

    #print "overwrote pbs_mom config file, its contents now are:"
    #FILE = open(PBS_MOM_CONF_FILE,'r')
    #for line in FILE: print line + "\n"
    #FILE.close()

    #overwrite $(TORQUECFG)/server_name file with fqdn of pbs_server
    #FILE = open(PBS_SERVER_NAME_FILE,'w')
    #FILE.write(self.pbs_server_fqdn)
    #FILE.close()

    ##try killing pbs_mom in case we changed the config
    #rval = Popen("momctl -s",shell=True).wait()
    #print "rval of momctl -s command was " + str(rval)
    #if rval != 0:
    #  print "tried to kill pbs_mom, but momctl -s command failed, prob because no mom was running"
    #else:
    #  time.sleep(1) #not sure if necessary, but wait a sec to be sure the mom lock file is deleted 

    ##run pbs_mom
    print "running pbs_mom on compute node"
    Popen("pbs_mom", shell=True)

  #def killTask(self, driver, tid):
    #send a message back to the scheduler to tell it this task is dead
    #msg = mesos.TaskStatus(tid, mesos.TASK_KILLED, "")
    #driver.sendStatusUpdate(msg);

  def shutdown(self, driver):
    print "shutdown"
    #cleanup()

  def error(self, driver, code, message):
    print "Error: %s" % message

if __name__ == "__main__":
  print "Starting pbs_mom executor"
  atexit.register(cleanup)
  executor = MyExecutor()
  mesos.MesosExecutorDriver(executor).run()
