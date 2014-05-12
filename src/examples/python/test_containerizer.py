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

# The scheme an external containerizer has to adhere to is;
#
# COMMAND < INPUT-PROTO > RESULT-PROTO
#
# launch < Launch
# update < Update
# usage < Usage > ResourceStatistics
# wait < Wait > Termination
# destroy < Destroy
# containers > Containers
# recover
#
# 'wait' is expected to block until the task command/executor has
# terminated.

import fcntl
import multiprocessing
import os
import signal
import subprocess
import sys
import struct
import time
import google
import mesos
import mesos_pb2
import containerizer_pb2


# Render a string describing how to use this script.
def use(argv0, methods):
    out = "Usage: %s <command>\n" % argv0
    out += "Valid commands: " + ', '.join(methods)

    return out


# Read a data chunk prefixed by its total size from stdin.
def receive():
    # Read size (uint32 => 4 bytes).
    size = struct.unpack('I', sys.stdin.read(4))
    if size[0] <= 0:
        print >> sys.stderr, "Expected protobuf size over stdin. " \
                             "Received 0 bytes."
        return ""

    # Read payload.
    data = sys.stdin.read(size[0])
    if len(data) != size[0]:
        print >> sys.stderr, "Expected %d bytes protobuf over stdin. " \
                             "Received %d bytes." % (size[0], len(data))
        return ""

    return data


# Write a protobuf message prefixed by its total size (aka recordio)
# to stdout.
def send(data):
    # Write size (uint32 => 4 bytes).
    sys.stdout.write(struct.pack('I', len(data)))

    # Write payload.
    sys.stdout.write(data)


# Start a containerized executor. Expects to receive an Launch
# protobuf via stdin.
def launch():
    try:
        data = receive()
        if len(data) == 0:
            return 1

        launch = containerizer_pb2.Launch()
        launch.ParseFromString(data)

        if launch.task_info.HasField("executor"):
            command = ["sh",
                       "-c",
                       launch.task_info.executor.command.value]
        else:
            print >> sys.stderr, "No executor passed; using mesos-executor!"
            executor = os.path.join(os.environ['MESOS_LIBEXEC_DIRECTORY'],
                                    "mesos-executor")
            command = ["sh",
                       "-c",
                       executor]
            print >> sys.stderr, "command " + str(command)

        lock_dir = os.path.join("/tmp/mesos-test-containerizer",
                                launch.container_id.value)
        subprocess.check_call(["mkdir", "-p", lock_dir])

        # Fork a child process for allowing a blocking wait.
        pid = os.fork()
        if pid == 0:
            # We are in the child.
            proc = subprocess.Popen(command, env=os.environ.copy())

            # Wait and serialize the process status when done.
            lock = os.path.join(lock_dir, "wait")
            with open(lock, "w+") as lk:
                fcntl.flock(lk, fcntl.LOCK_EX)

                status = proc.wait()

                lk.write(str(status) + "\n")

            sys.exit(status)
        else:
            # We are in the parent.

            # Serialize the subprocess pid.
            lock = os.path.join(lock_dir, "pid")
            with open(lock, "w+") as lk:
                fcntl.flock(lk, fcntl.LOCK_EX)

                lk.write(str(pid) + "\n")

    except google.protobuf.message.DecodeError:
        print >> sys.stderr, "Could not deserialise Launch protobuf"
        return 1

    except OSError as e:
        print >> sys.stderr, e.strerror
        return 1

    except ValueError:
        print >> sys.stderr, "Value is invalid"
        return 1

    return 0


# Update the container's resources.
# Expects to receive a Update protobuf via stdin.
def update():
    try:
        data = receive()
        if len(data) == 0:
            return 1

        update = containerizer_pb2.Update()
        update.ParseFromString(data)

        print >> sys.stderr, "Received "                \
                           + str(len(update.resources)) \
                           + " resource elements."

    except google.protobuf.message.DecodeError:
        print >> sys.stderr, "Could not deserialise Update protobuf."
        return 1

    except OSError as e:
        print >> sys.stderr, e.strerror
        return 1

    except ValueError:
        print >> sys.stderr, "Value is invalid"
        return 1

    return 0


# Gather resource usage statistics for the containerized executor.
# Delivers an ResourceStatistics protobut via stdout when
# successful.
def usage():
    try:
        data = receive()
        if len(data) == 0:
            return 1
        usage = containerizer_pb2.Usage()
        usage.ParseFromString(data)

        statistics = mesos_pb2.ResourceStatistics()

        statistics.timestamp = time.time()

        # Return hardcoded dummy statistics.
        # TODO(tillt): Make use of mesos-usage here for capturing real
        # statistics.
        statistics.mem_rss_bytes = 1073741824
        statistics.mem_limit_bytes = 1073741824
        statistics.cpus_limit = 2
        statistics.cpus_user_time_secs = 0.12
        statistics.cpus_system_time_secs = 0.5

        send(statistics.SerializeToString())

    except google.protobuf.message.DecodeError:
        print >> sys.stderr, "Could not deserialise Usage protobuf."
        return 1

    except google.protobuf.message.EncodeError:
        print >> sys.stderr, "Could not serialise ResourceStatistics protobuf."
        return 1

    except OSError as e:
        print >> sys.stderr, e.strerror
        return 1

    return 0


# Terminate the containerized executor.
def destroy():
    try:
        data = receive()
        if len(data) == 0:
            return 1
        destroy = containerizer_pb2.Destroy()
        destroy.ParseFromString(data)

        lock_dir = os.path.join("/tmp/mesos-test-containerizer",
                                destroy.container_id.value)
        lock = os.path.join(lock_dir, "pid")

        # Obtain our shared lock once it becomes available, read
        # the pid and kill that process.
        with open(lock, "r") as lk:
            fcntl.flock(lk, fcntl.LOCK_SH)

            pid = int(lk.read())

            os.kill(pid, signal.SIGKILL)

    except google.protobuf.message.DecodeError:
        print >> sys.stderr, "Could not deserialise Destroy protobuf."
        return 1

    except OSError as e:
        print >> sys.stderr, e.strerror
        return 1

    return 0


# Recover all containerized executors states.
def recover():

    # This currently does not try to recover any internal state and
    # therefore is to be regarded as being not complete.
    # A complete implementation would attempt to recover all active
    # containers by deserializing all previously checkpointed
    # ContainerIDs.

    return 0


# Get the containerized executor's Termination.
# Delivers a Termination protobuf filled with the information
# gathered from launch's wait via stdout.
def wait():
    try:
        data = receive()
        if len(data) == 0:
            return 1
        wait = containerizer_pb2.Wait()
        wait.ParseFromString(data)

        lock_dir = os.path.join("/tmp/mesos-test-containerizer",
                                wait.container_id.value)
        lock = os.path.join(lock_dir, "wait")

        # Obtain our shared lock once it becomes available and read
        # the status code.
        with open(lock, "r") as lk:
            fcntl.flock(lk, fcntl.LOCK_SH)
            status = int(lk.read())

        # Deliver the termination protobuf back to the slave.
        termination = containerizer_pb2.Termination()
        termination.killed = false
        termination.status = status
        termination.message = ""

        send(termination.SerializeToString())

    except google.protobuf.message.DecodeError:
        print >> sys.stderr, "Could not deserialise Termination protobuf."
        return 1

    except google.protobuf.message.EncodeError:
        print >> sys.stderr, "Could not serialise Termination protobuf."
        return 1

    except OSError as e:
        print >> sys.stderr, e.strerror
        return 1

    return 0


def containers():
    try:
        containers = containerizer_pb2.Containers()

        # This currently does not fill in any active containers and
        # therefore is to be regarded as being not complete.
        # A complete implementation would fill the containers message
        # with all active ContainerIDs.

        send(containers.SerializeToString())

    except google.protobuf.message.EncodeError:
        print >> sys.stderr, "Could not serialise Containers protobuf."
        return 1

    except OSError as e:
        print >> sys.stderr, e.strerror
        return 1

    return 0


if __name__ == "__main__":
    methods = { "launch":       launch,
                "update":       update,
                "destroy":      destroy,
                "containers":   containers,
                "recover":      recover,
                "usage":        usage,
                "wait":         wait }

    if sys.argv[1:2] == ["--help"] or sys.argv[1:2] == ["-h"]:
        print use(sys.argv[0], methods.keys())
        sys.exit(0)

    if len(sys.argv) < 2:
        print >> sys.stderr, "Please pass a command"
        print >> sys.stderr, use(sys.argv[0], methods.keys())
        sys.exit(1)

    command = sys.argv[1]
    if command not in methods:
        print >> sys.stderr, "Invalid command passed"
        print >> sys.stderr, use(sys.argv[0], methods.keys())
        sys.exit(2)

    method = methods.get(command)

    sys.exit(method())
