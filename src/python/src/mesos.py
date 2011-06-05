import sys
import _mesos


# Base class for Mesos schedulers. Users' schedulers should extend this class
# to get default implementations of methods they don't override.
class Scheduler:
  def getFrameworkName(self, driver): pass
  def getExecutorInfo(self, driver): pass
  def registered(self, driver, frameworkId): pass
  def resourceOffer(self, driver, offerId, offers): pass
  def statusUpdate(self, driver, status): pass
  def frameworkMessage(self, driver, message): pass
  def slaveLost(self, driver, slaveId): pass

  # Default implementation of error() prints to stderr because we can't
  # make error() an abstract method in Python
  def error(self, driver, code, message):
    print >> sys.stderr, "Error from Mesos: %s (code: %d)" % (message, code)


# Interface for Mesos scheduler drivers. Users may wish to extend this class
# in mock objects for tests.
class SchedulerDriver:
  def start(self): pass
  def stop(self): pass
  def join(self): pass
  def run(self): pass
  def sendFrameworkMessage(self, message): pass
  def killTask(self, taskId): pass
  def replyToOffer(self, offerId, tasks, params = None): pass
  def reviveOffers(self): pass
