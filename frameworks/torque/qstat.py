import xml.dom.minidom

from subprocess import *

class Job:
  def __init__(self, xmlJobElt):
    print "creating a new Job obj out of xml"
    print "xml element that was passed should be named 'Job', is it?: " + xmlJobElt.nodeName
    self.resourceList = {}
    for res in xmlJobElt.getElementsByTagName("Resource_List")[0].childNodes:
      self.resourceList[res.nodeName] = res.childNodes[0].data

def getActiveJobs():
  print "in getJobs, grabbing xml output from qstat"
  jobs = Popen("qstat -x", shell=True, stdout=PIPE).stdout
  #print "output of qstat: "
  #for line in qstat:
  #  print line
  dom_doc = xml.dom.minidom.parse(jobs)
  print "grabbing the Job elements from the xml dom doc"
  xmljobs = dom_doc.getElementsByTagName("Job")
  jobs = []
  print "creating a new job object for each job dom elt"
  for j in xmljobs:
    jobs.append(Job(j))
  return jobs

#TODO: DELETE THIS? Might note be used eventually
def getQueueLength():
  #print "computing the number of active jobs in the queue"
  qstat = Popen("qstat -Q",shell=True,stdout=PIPE).stdout
  jobcount = 0
  for line in qstat:
     if re.match('^batch.*', line):
       jobcount = int(line.split()[5]) + int(line.split()[6]) + int(line.spli
  return jobcount
