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

# Helper for printing out a message and then the "usage" then exiting.
def usage(message, parser):
    import sys
    sys.stderr.write(message + '\n')
    parser.print_help()
    sys.exit(1)


# Helper for printing out a message and then exiting.
def fatal(message):
    import sys
    sys.stderr.write(message + '\n')
    sys.exit(1)


# Helper that uses 'mesos-resolve' to resolve a master IP:port from
# one of:
#     zk://host1:port1,host2:port2,.../path
#     zk://username:password@host1:port1,host2:port2,.../path
#     file://path/to/file (where file contains one of the above)
def resolve(master):
    import subprocess

    process = subprocess.Popen(
        ['mesos-resolve', master],
        stdin=None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        shell=False)

    status = process.wait()
    if status != 0:
        raise Exception('Failed to execute \'mesos-resolve %s\':\n%s'
                        % (master, process.stderr.read()))

    result = process.stdout.read().strip()
    process.stdout.close()
    process.stderr.close()
    return result
