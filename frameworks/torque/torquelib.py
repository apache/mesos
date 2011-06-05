import re
import tempfile
import logging
import xml.dom.minidom

from subprocess import *

logging.basicConfig()

class Job:
  def __init__(self, xmlJobElt):
    #logging.debug("creating a new Job obj out of xml")
    assert xmlJobElt.nodeName == "Job", "xml element passed to Job constructor was not named 'Job'"
    self.resourceList = {}
    for res in xmlJobElt.getElementsByTagName("Resource_List")[0].childNodes:
      self.resourceList[res.nodeName] = res.childNodes[0].data
    self.jobState = xmlJobElt.getElementsByTagName("job_state")[0].childNodes[0].data

def getActiveJobs():
  logging.debug("in getJobs, grabbing xml output from qstat")
  qstat_out = tempfile.TemporaryFile()
  qstat_obj = Popen("qstat -x", shell=True, stdout=qstat_out)
  qstat_obj.wait()
  logging.debug("output of qstat: ")
  jobs = []
  jobsExist = False
  qstat_out.seek(0)
  for line in qstat_out:
    if re.match(".*Job.*", line):
      #logging.debug("#############" + line + "#############")
      jobsExist = True
      break
  if jobsExist:
    qstat_out.seek(0)
    dom_doc = xml.dom.minidom.parse(qstat_out)
    logging.debug("grabbing the Job elements from the xml dom doc")
    xmljobs = dom_doc.getElementsByTagName("Job")
    logging.debug("creating a new job object for each job dom elt")
    for j in xmljobs:
      #make sure job's state is not complete
      newJob = Job(j)
      if newJob.jobState != "C":
        jobs.append(newJob)
  if len(jobs) == 0:
    logging.debug("the string \"Job\" was not found in the qstat -x command output")
  return jobs

class Node:
  def __init__(self, xmlHostElt):
    #logging.debug("creating a new Host obj out of xml")
    assert xmlHostElt.nodeName == "Node", "xml element passed to Node constructor was not named 'Node'"
    self.name = xmlHostElt.getElementsByTagName("name")[0].childNodes[0].data

    self.state = [] 
    for stateElt in xmlHostElt.getElementsByTagName("state"):
      assert stateElt.nodeName == "state"
      for st in stateElt.childNodes[0].data.split(","):
        self.state.append(st)

    self.np = xmlHostElt.getElementsByTagName("np")[0].childNodes[0].data
    self.ntype = xmlHostElt.getElementsByTagName("ntype")[0].childNodes[0].data

    self.status = {}
    for statusElt in xmlHostElt.getElementsByTagName("status"):
      assert statusElt.nodeName == "status"
      for pairs in statusElt.childNodes[0].data.split(","):
        key, val = pairs.split("=")
        self.status[key] = val

#returns nodes whose state is not marked as "down"
def getNodes():
  logging.debug("in getNodes, grabbing xml output from pbsnodes")
  pbsnodes_out = tempfile.TemporaryFile()
  pbsnodes_obj = Popen("pbsnodes -x", shell=True, stdout=pbsnodes_out)
  pbsnodes_obj.wait()
  nodes = []
  nodesExist = False
  pbsnodes_out.seek(0)
  for line in pbsnodes_out:
    #logging.debug("node line is %s" % line)
    if re.match(".*Node.*", line):
      nodesExist = True
      break
  if nodesExist:
    pbsnodes_out.seek(0)
    dom_doc = xml.dom.minidom.parse(pbsnodes_out)
    logging.debug("grabbing the Job elements from the xml dom doc")
    xmlnodes = dom_doc.getElementsByTagName("Node")
    logging.debug("creating a new node object for each node dom elt")
    for n in xmlnodes:
      #make sure node's state is online
      nodes.append(Node(n))
  if len(nodes) == 0:
    logging.debug("the string \"Node\" was not found in the pbsnodes -x command output")
  return nodes

#TODO: DELETE THIS? Might note be used eventually
def getQueueLength():
  #logging.debug("computing the number of active jobs in the queue")
  qstat = Popen("qstat -Q",shell=True,stdout=PIPE).stdout
  jobcount = 0
  for line in qstat:
     if re.match('^batch.*', line):
       jobcount = int(line.split()[5]) + int(line.split()[6]) + int(line.split()[7]) + int(line.split()[8])
  return jobcount
