#!/usr/bin/env python
import nexus
import random
import sys
import time
import os
import pickle

# Scheduler configurations as pairs of (todo, duration) to run.
config = [ (1, 10), (1, 10), (1, 10), (1, 10), (1, 10),
           (1, 10), (1, 10), (1, 10), (1, 10), (1, 10),
           (1, 10), (1, 10), (1, 10), (1, 10), (1, 10),
           (1, 10), (1, 10), (1, 10), (1, 10), (1, 10),
           (5, 10), (5, 10), (5, 10), (5, 10), (5, 10),
           (5, 10), (5, 10), (5, 10), (5, 10), (5, 10),
           (5, 10), (5, 10), (5, 10), (5, 10), (5, 10),
           (5, 10), (5, 10), (5, 10), (5, 10), (5, 10),
           (10, 1), (10, 1), (10, 1), (10, 1), (10, 1),
           (10, 1), (10, 1), (10, 1), (10, 1), (10, 1),
           (10, 1), (10, 1), (10, 1), (10, 1), (10, 1),
           (10, 1), (10, 1), (10, 1), (10, 1), (10, 1),
           (100, 1), (100, 1), (100, 1), (100, 1), (100, 1),
           (100, 1), (100, 1), (100, 1), (100, 1), (100, 1),
           (100, 1), (100, 1), (100, 1), (100, 1), (100, 1),
           (100, 1), (100, 1), (100, 1), (100, 1), (100, 1) ]


class ScalingScheduler(nexus.Scheduler):
  def __init__(self, master):
    nexus.Scheduler.__init__(self)
    self.tid = 0
    self.master = master
    self.running = {}

  def getFrameworkName(self, driver):
    return "Scaling Framework"

  def getExecutorInfo(self, driver):
    return nexus.ExecutorInfo("scaling_exec", os.getcwd(), "")

  def registered(self, driver, fid):
    print "Scaling Scheduler Registered!"

  def resourceOffer(self, driver, oid, offers):
    # Make sure the nested schedulers can actually run their tasks.
    if len(offers) <= len(config):
      print "Need at least one spare slave to do this work ... exiting!"
      driver.stop()

    # Farm out the schedulers!
    tasks = []
    for offer in offers:
      if len(config) != self.tid:
        (todo, duration) = config[self.tid]
        arg = pickle.dumps((self.master, (todo, duration)))
        task = nexus.TaskDescription(self.tid, offer.slaveId,
                                     "task %d" % self.tid, offer.params, arg)
        tasks.append(task)
        self.running[self.tid] = (todo, duration)
        self.tid += 1
    driver.replyToOffer(oid, tasks, {})

  def statusUpdate(self, driver, status):
    # For now, we are expecting our tasks to be lost ...
    if status.state == nexus.TASK_LOST:
      todo, duration = self.running[status.taskId]
      print "Finished %d todo at %d secs" % (todo, duration)
      del self.running[status.taskId]
      if self.tid == len(config) and len(self.running) == 0:
        driver.stop()


if __name__ == "__main__":
  if sys.argv[1] == "local" or sys.argv[1] == "localquiet":
      print "Cannot do scaling experiments with 'local' or 'localquiet'!"
      sys.exit(1)

  nexus.NexusSchedulerDriver(ScalingScheduler(sys.argv[1]), sys.argv[1]).run()
