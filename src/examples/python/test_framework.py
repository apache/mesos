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

import os
import sys
import time

import mesos.interface
from mesos.interface import mesos_pb2
from mesos.scheduler import MesosSchedulerDriver

TOTAL_TASKS = 5

TASK_CPUS = 1
TASK_MEM = 128

class TestScheduler(mesos.interface.Scheduler):
    def __init__(self, implicitAcknowledgements, executor, framework):
        self.implicitAcknowledgements = implicitAcknowledgements
        self.executor = executor
        self.framework = framework
        self.taskData = {}
        self.tasksLaunched = 0
        self.tasksFinished = 0
        self.messagesSent = 0
        self.messagesReceived = 0

    def registered(self, driver, frameworkId, masterInfo):
        print "Registered with framework ID %s" % frameworkId.value
        self.framework.id.CopyFrom(frameworkId)
        driver.updateFramework(framework, [])

    def resourceOffers(self, driver, offers):
        for offer in offers:
            tasks = []
            offerCpus = 0
            offerMem = 0
            for resource in offer.resources:
                if resource.name == "cpus":
                    offerCpus += resource.scalar.value
                elif resource.name == "mem":
                    offerMem += resource.scalar.value

            print "Received offer %s with cpus: %s and mem: %s" \
                  % (offer.id.value, offerCpus, offerMem)

            remainingCpus = offerCpus
            remainingMem = offerMem

            while self.tasksLaunched < TOTAL_TASKS and \
                  remainingCpus >= TASK_CPUS and \
                  remainingMem >= TASK_MEM:
                tid = self.tasksLaunched
                self.tasksLaunched += 1

                print "Launching task %d using offer %s" \
                      % (tid, offer.id.value)

                task = mesos_pb2.TaskInfo()
                task.task_id.value = str(tid)
                task.slave_id.value = offer.slave_id.value
                task.name = "task %d" % tid
                task.executor.MergeFrom(self.executor)

                cpus = task.resources.add()
                cpus.name = "cpus"
                cpus.type = mesos_pb2.Value.SCALAR
                cpus.scalar.value = TASK_CPUS

                mem = task.resources.add()
                mem.name = "mem"
                mem.type = mesos_pb2.Value.SCALAR
                mem.scalar.value = TASK_MEM

                tasks.append(task)
                self.taskData[task.task_id.value] = (
                    offer.slave_id, task.executor.executor_id)

                remainingCpus -= TASK_CPUS
                remainingMem -= TASK_MEM

            operation = mesos_pb2.Offer.Operation()
            operation.type = mesos_pb2.Offer.Operation.LAUNCH
            operation.launch.task_infos.extend(tasks)

            driver.acceptOffers([offer.id], [operation])

    def statusUpdate(self, driver, update):
        print "Task %s is in state %s" % \
            (update.task_id.value, mesos_pb2.TaskState.Name(update.state))

        # Ensure the binary data came through.
        if update.data != "data with a \0 byte":
            print "The update data did not match!"
            print "  Expected: 'data with a \\x00 byte'"
            print "  Actual:  ", repr(str(update.data))
            sys.exit(1)

        if update.state == mesos_pb2.TASK_FINISHED:
            self.tasksFinished += 1
            if self.tasksFinished == TOTAL_TASKS:
                print "All tasks done, waiting for final framework message"

            slave_id, executor_id = self.taskData[update.task_id.value]

            self.messagesSent += 1
            driver.sendFrameworkMessage(
                executor_id,
                slave_id,
                'data with a \0 byte')

        if update.state == mesos_pb2.TASK_LOST or \
           update.state == mesos_pb2.TASK_KILLED or \
           update.state == mesos_pb2.TASK_FAILED:
            print "Aborting because task %s is in unexpected state %s with message '%s'" \
                % (update.task_id.value, mesos_pb2.TaskState.Name(update.state), update.message)
            driver.abort()

        # Explicitly acknowledge the update if implicit acknowledgements
        # are not being used.
        if not self.implicitAcknowledgements:
            driver.acknowledgeStatusUpdate(update)

    def frameworkMessage(self, driver, executorId, slaveId, message):
        self.messagesReceived += 1

        # The message bounced back as expected.
        if message != "data with a \0 byte":
            print "The returned message data did not match!"
            print "  Expected: 'data with a \\x00 byte'"
            print "  Actual:  ", repr(str(message))
            sys.exit(1)
        print "Received message:", repr(str(message))

        if self.messagesReceived == TOTAL_TASKS:
            if self.messagesReceived != self.messagesSent:
                print "Sent", self.messagesSent,
                print "but received", self.messagesReceived
                sys.exit(1)
            print "All tasks done, and all messages received, exiting"
            driver.stop()

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print "Usage: %s master" % sys.argv[0]
        sys.exit(1)

    executor = mesos_pb2.ExecutorInfo()
    executor.executor_id.value = "default"
    executor.command.value = os.path.abspath("./test-executor")
    executor.name = "Test Executor (Python)"
    executor.source = "python_test"

    framework = mesos_pb2.FrameworkInfo()
    framework.user = "" # Have Mesos fill in the current user.
    framework.name = "Test Framework (Python)"
    framework.checkpoint = True
    framework.role = "*"

    implicitAcknowledgements = 1
    if os.getenv("MESOS_EXPLICIT_ACKNOWLEDGEMENTS"):
        print "Enabling explicit status update acknowledgements"
        implicitAcknowledgements = 0

    if os.getenv("MESOS_EXAMPLE_AUTHENTICATE"):
        print "Enabling authentication for the framework"

        if not os.getenv("MESOS_EXAMPLE_PRINCIPAL"):
            print "Expecting authentication principal in the environment"
            sys.exit(1);

        credential = mesos_pb2.Credential()
        credential.principal = os.getenv("MESOS_EXAMPLE_PRINCIPAL")

        if os.getenv("MESOS_EXAMPLE_SECRET"):
            credential.secret = os.getenv("MESOS_EXAMPLE_SECRET")

        framework.principal = os.getenv("MESOS_EXAMPLE_PRINCIPAL")

    else:
        framework.principal = "test-framework-python"
        credential = None

    # Subscribe with all roles suppressed to test updateFramework() method
    driver = MesosSchedulerDriver(
        TestScheduler(implicitAcknowledgements, executor, framework),
        framework,
        sys.argv[1],
        implicitAcknowledgements,
        credential,
        [framework.role])

    status = 0 if driver.run() == mesos_pb2.DRIVER_STOPPED else 1

    # Ensure that the driver process terminates.
    driver.stop();

    sys.exit(status)
