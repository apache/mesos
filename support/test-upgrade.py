#!/usr/bin/env python3
#
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

"""Script to test the upgrade path between two versions of Mesos."""

import argparse
import os
import subprocess
import sys
import tempfile
import time

DEFAULT_PRINCIPAL = 'foo'
DEFAULT_SECRET = 'bar'


class Process():
    """
    Helper class to keep track of process lifecycles.

    This class allows to start processes, capture their
    output, and check their liveness during delays/sleep.
    """

    def __init__(self, args, environment=None):
        """Initialize the Process."""
        outfile = tempfile.mktemp()
        fout = open(outfile, 'w')
        print('Run %s, output: %s' % (args, outfile))

        # TODO(nnielsen): Enable glog verbose logging.
        self.process = subprocess.Popen(args,
                                        stdout=fout,
                                        stderr=subprocess.STDOUT,
                                        env=environment)

    def sleep(self, seconds):
        """
        Poll the process for the specified number of seconds.

        If the process ends during that time, this method returns the process's
        return value. If the process is still running after that time period,
        this method returns `True`.
        """
        poll_time = 0.1
        while seconds > 0:
            seconds -= poll_time
            time.sleep(poll_time)
            poll = self.process.poll()
            if poll is not None:
                return poll
        return True

    def __del__(self):
        """Kill the Process."""
        if self.process.poll() is None:
            self.process.kill()


class Agent(Process):
    """Class representing an agent process."""

    def __init__(self, path, work_dir, credfile):
        """Initialize a Mesos agent by running mesos-slave.sh."""
        Process.__init__(self, [os.path.join(path, 'bin', 'mesos-slave.sh'),
                                '--master=127.0.0.1:5050',
                                '--credential=' + credfile,
                                '--work_dir=' + work_dir,
                                '--resources=disk:2048;mem:2048;cpus:2'])


class Master(Process):
    """Class representing a master process."""

    def __init__(self, path, work_dir, credfile):
        """Initialize a Mesos master by running mesos-master.sh."""
        Process.__init__(self, [os.path.join(path, 'bin', 'mesos-master.sh'),
                                '--ip=127.0.0.1',
                                '--work_dir=' + work_dir,
                                '--authenticate',
                                '--credentials=' + credfile,
                                '--roles=test'])


# TODO(greggomann): Add support for multiple frameworks.
class Framework(Process):
    """Class representing a framework instance (the test-framework for now)."""

    def __init__(self, path):
        """Initialize a framework."""
        # The test-framework can take these parameters as environment variables,
        # but not as command-line parameters.
        environment = {
            # In Mesos 0.28.0, the `MESOS_BUILD_DIR` environment variable in the
            # test framework was changed to `MESOS_HELPER_DIR`, and the '/src'
            # subdirectory was added to the variable's path. Both are included
            # here for backwards compatibility.
            'MESOS_BUILD_DIR': path,
            'MESOS_HELPER_DIR': os.path.join(path, 'src'),
            # MESOS_AUTHENTICATE is deprecated in favor of
            # MESOS_AUTHENTICATE_FRAMEWORKS, although 0.28.x still expects
            # previous one, therefore adding both.
            'MESOS_AUTHENTICATE': '1',
            'MESOS_AUTHENTICATE_FRAMEWORKS': '1',
            'DEFAULT_PRINCIPAL': DEFAULT_PRINCIPAL,
            'DEFAULT_SECRET': DEFAULT_SECRET
        }

        Process.__init__(self, [os.path.join(path, 'src', 'test-framework'),
                                '--master=127.0.0.1:5050'], environment)


def version(path):
    """Get the Mesos version from the built executables."""
    mesos_master_path = os.path.join(path, 'bin', 'mesos-master.sh')
    process = subprocess.Popen([mesos_master_path, '--version'],
                               stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    output, _ = process.communicate()
    return_code = process.returncode
    if return_code != 0:
        return False

    return output[:-1]


def create_master(master_version, build_path, work_dir, credfile):
    """Create a master using a specific version."""
    print('##### Starting %s master #####' % master_version)
    master = Master(build_path, work_dir, credfile)
    if not master.sleep(0.5):
        print('%s master exited prematurely' % master_version)
        sys.exit(1)
    return master


def create_agent(agent_version, build_path, work_dir, credfile):
    """Create an agent using a specific version."""
    print('##### Starting %s agent #####' % agent_version)
    agent = Agent(build_path, work_dir, credfile)
    if not agent.sleep(0.5):
        print('%s agent exited prematurely' % agent_version)
        sys.exit(1)
    return agent


def test_framework(framework_version, build_path):
    """Run a version of the test framework on a specified version of Mesos."""
    print('##### Starting %s framework #####' % framework_version)
    print('Waiting for %s framework to complete (10 sec max)...' % (
        framework_version))
    framework = Framework(build_path)
    if framework.sleep(10) != 0:
        print('%s framework failed' % framework_version)
        sys.exit(1)


# TODO(nnielsen): Add support for zookeeper and failover of master.
# TODO(nnielsen): Add support for testing scheduler live upgrade/failover.
def main():
    """Main function to test the upgrade between two Mesos builds."""
    parser = argparse.ArgumentParser(
        description='Test upgrade path between two mesos builds')
    parser.add_argument('--prev',
                        type=str,
                        help='Build path to mesos version to upgrade from',
                        required=True)

    parser.add_argument('--next',
                        type=str,
                        help='Build path to mesos version to upgrade to',
                        required=True)
    args = parser.parse_args()

    # Get the version strings from the built executables.
    prev_version = version(args.prev)
    next_version = version(args.__next__)

    if not prev_version or not next_version:
        print('Could not get mesos version numbers')
        sys.exit(1)

    # Write credentials to temporary file.
    credfile = tempfile.mktemp()
    with open(credfile, 'w') as fout:
        fout.write(DEFAULT_PRINCIPAL + ' ' + DEFAULT_SECRET)

    # Create a work directory for the master.
    master_work_dir = tempfile.mkdtemp()

    # Create a work directory for the agent.
    agent_work_dir = tempfile.mkdtemp()

    print('Running upgrade test from %s to %s' % (prev_version, next_version))

    print("""\
+--------------+----------------+----------------+---------------+
| Test case    |   Framework    |     Master     |     Agent     |
+--------------+----------------+----------------+---------------+
|    #1        |  %s\t| %s\t | %s\t |
|    #2        |  %s\t| %s\t | %s\t |
|    #3        |  %s\t| %s\t | %s\t |
|    #4        |  %s\t| %s\t | %s\t |
+--------------+----------------+----------------+---------------+

NOTE: live denotes that master process keeps running from previous case.
    """ % (prev_version, prev_version, prev_version,
           prev_version, next_version, prev_version,
           prev_version, next_version, next_version,
           next_version, next_version, next_version))

    # Test case 1.
    master = create_master(prev_version, args.prev, master_work_dir, credfile)
    agent = create_agent(prev_version, args.prev, agent_work_dir, credfile)
    test_framework(prev_version, args.prev)

    # Test case 2.
    # NOTE: Need to stop and start the agent because standalone detector does
    # not detect master failover.
    agent.process.kill()
    master.process.kill()
    master = create_master(next_version, args.__next__, master_work_dir,
                           credfile)
    agent = create_agent(prev_version, args.prev, agent_work_dir, credfile)
    test_framework(prev_version, args.prev)

    # Test case 3.
    agent.process.kill()
    agent = create_agent(next_version, args.__next__, agent_work_dir, credfile)
    test_framework(prev_version, args.prev)

    # Test case 4.
    test_framework(next_version, args.__next__)

    # Tests passed.
    sys.exit(0)

if __name__ == '__main__':
    main()
