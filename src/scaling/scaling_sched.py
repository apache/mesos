#!/usr/bin/env python

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
# 
#     http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import mesos
import random
import sys
import time
import os
import pickle

CPUS = 1
MEM = 50*1024*1024

config1 = [ (1,20) ]

config2 = [ (1,20), (1,240) ]

config = [ (50, 120),
           (50, 120),
           (50, 120),
           (50, 120),
           (50, 120),
           (50, 120),
           (50, 120),
           (50, 120),
           (50, 120),
           (50, 120) ]

class ScalingScheduler(mesos.Scheduler):
  def __init__(self, master):
    mesos.Scheduler.__init__(self)
    self.tid = 0
    self.master = master
    print self.master
    self.running = {}

  def getFrameworkName(self, driver):
    return "Scaling Framework"

  def getExecutorInfo(self, driver):
    execPath = os.path.join(os.getcwd(), "scaling_exec")
    return mesos.ExecutorInfo(execPath, "")

  def registered(self, driver, fid):
    print "Scaling Scheduler Registered!"

  def resourceOffer(self, driver, oid, offers):
    # Make sure the nested schedulers can actually run their tasks.
    # if len(offers) <= len(config) and len(config) != self.tid:
    #   print "Need at least one spare agent to do this work ... exiting!"
    #   driver.stop()
    #   return

    # Farm out the schedulers!
    tasks = []
    for offer in offers:
      if len(config) != self.tid:
        (todo, duration) = config[self.tid]
        arg = pickle.dumps((self.master, (todo, duration)))
        pars = {"cpus": "%d" % CPUS, "mem": "%d" % MEM}
        task = mesos.TaskInfo(self.tid, offer.slaveId,
                              "task %d" % self.tid, pars, arg)
        tasks.append(task)
        self.running[self.tid] = (todo, duration)
        self.tid += 1
        print "Launching (%d, %d) on agent %s" % (todo, duration, offer.slaveId)
    driver.launchTasks(oid, tasks)

  def statusUpdate(self, driver, status):
    # For now, we are expecting our tasks to be lost ...
    if status.state == mesos.TASK_LOST:
      todo, duration = self.running[status.taskId]
      print "Finished %d todo at %d secs" % (todo, duration)
      del self.running[status.taskId]
      if self.tid == len(config) and len(self.running) == 0:
        driver.stop()


if __name__ == "__main__":
  if sys.argv[1] == "local" or sys.argv[1] == "localquiet":
      print "Cannot do scaling experiments with 'local' or 'localquiet'!"
      sys.exit(1)

  mesos.MesosSchedulerDriver(ScalingScheduler(sys.argv[1]), sys.argv[1]).run()
