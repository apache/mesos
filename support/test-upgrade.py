#!/usr/bin/env python

import argparse
import os
import subprocess
import sys
import tempfile
import time

DEFAULT_PRINCIPAL = 'foo'
DEFAULT_SECRET = 'bar'

# Helper class to keep track of process lifecycles, i.e., starting processes,
# capturing output, and checking liveness during delays/sleep.
class Process:

    def __init__(self, args, environment=None):
        outfile = tempfile.mktemp()
        fout = open(outfile, 'w')
        print 'Run %s, output: %s' % (args, outfile)

        # TODO(nnielsen): Enable glog verbose logging.
        self.process = subprocess.Popen(args,
                                        stdout=fout,
                                        stderr=subprocess.STDOUT,
                                        env=environment)

    # Polls the process for the specified number of seconds, returning the
    # process's return value if it ends during that time. Returns `True` if the
    # process is still running after that time period.
    def sleep(self, seconds):
        poll_time = 0.1
        while seconds > 0:
            seconds -= poll_time
            time.sleep(poll_time)
            poll = self.process.poll()
            if poll != None:
                return poll
        return True

    def __del__(self):
        if self.process.poll() == None:
            self.process.kill()


# Class representing a agent process.
class Agent(Process):

    def __init__(self, path, work_dir, credfile):
        Process.__init__(self, [os.path.join(path, 'bin', 'mesos-slave.sh'),
                                '--master=127.0.0.1:5050',
                                '--credential=' + credfile,
                                '--work_dir=' + work_dir,
                                '--resources=disk:2048;mem:2048;cpus:2'])
        pass


# Class representing a master process.
class Master(Process):

    def __init__(self, path, work_dir, credfile):
        Process.__init__(self, [os.path.join(path, 'bin', 'mesos-master.sh'),
                                '--ip=127.0.0.1',
                                '--work_dir=' + work_dir,
                                '--authenticate',
                                '--credentials=' + credfile,
                                '--roles=test'])
        pass


# Class representing a framework instance (the test-framework for now).
#
# TODO(greggomann): Add support for multiple frameworks.
class Framework(Process):

    def __init__(self, path):
        # The test-framework can take these parameters as environment variables,
        # but not as command-line parameters.
        environment = {
            # In Mesos 0.28.0, the `MESOS_BUILD_DIR` environment variable in the
            # test framework was changed to `MESOS_HELPER_DIR`, and the '/src'
            # subdirectory was added to the variable's path. Both are included
            # here for backwards compatibility.
            'MESOS_BUILD_DIR': path,
            'MESOS_HELPER_DIR': os.path.join(path, 'src'),
            'MESOS_AUTHENTICATE': '1',
            'DEFAULT_PRINCIPAL': DEFAULT_PRINCIPAL,
            'DEFAULT_SECRET': DEFAULT_SECRET
        }

        Process.__init__(self, [os.path.join(path, 'src', 'test-framework'),
                                '--master=127.0.0.1:5050'], environment)
        pass


# Convenience function to get the Mesos version from the built executables.
def version(path):
    p = subprocess.Popen([os.path.join(path, 'bin', 'mesos-master.sh'),
                          '--version'],
                         stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    output, err = p.communicate()
    rc = p.returncode
    if rc != 0:
        return False

    return output[:-1]


# Script to test the upgrade path between two versions of Mesos.
#
# TODO(nnielsen): Add support for zookeeper and failover of master.
# TODO(nnielsen): Add support for testing scheduler live upgrade/failover.
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Test upgrade path between two mesos builds')
    parser.add_argument('--prev',
                        type=str,
                        help='Build path to mesos version to upgrade from',
                        required=True)

    parser.add_argument('--next',
                        type=str,
                        help='Build path to mesos version to upgrade to',
                        required=True)
    args = parser.parse_args()

    prev_path = args.prev
    next_path = args.next

    # Get the version strings from the built executables.
    prev_version = version(prev_path)
    next_version = version(next_path)

    if prev_version == False or next_version == False:
        print 'Could not get mesos version numbers'
        sys.exit(1)

    # Write credentials to temporary file.
    credfile = tempfile.mktemp()
    with open(credfile, 'w') as fout:
        fout.write(DEFAULT_PRINCIPAL + ' ' + DEFAULT_SECRET)

    # Create a work directory for the master.
    master_work_dir = tempfile.mkdtemp()

    # Create a work directory for the agent.
    agent_work_dir = tempfile.mkdtemp()

    print 'Running upgrade test from %s to %s' % (prev_version, next_version)

    print """\
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
           next_version, next_version, next_version)

    print ''
    print 'Test case 1 (Run of previous setup)'
    print '##### Starting %s master #####' % prev_version

    prev_master = Master(prev_path, master_work_dir, credfile)
    if prev_master.sleep(0.5) != True:
        print '%s master exited prematurely' % prev_version
        sys.exit(1)

    print '##### Starting %s agent #####' % prev_version

    prev_agent = Agent(prev_path, agent_work_dir, credfile)
    if prev_agent.sleep(0.5) != True:
        print '%s agent exited prematurely' % prev_version
        sys.exit(1)

    print '##### Starting %s framework #####' % prev_version
    print 'Waiting for %s framework to complete (10 sec max)...' % prev_version
    prev_framework = Framework(prev_path)
    if prev_framework.sleep(10) != 0:
        print '%s framework failed' % prev_version
        sys.exit(1)


    print ''
    print 'Test case 2 (Upgrade master)'

    # NOTE: Need to stop and start the agent because standalone detector does
    # not detect master failover.
    print '##### Stopping %s agent #####' % prev_version
    prev_agent.process.kill()

    print '##### Stopping %s master #####' % prev_version
    prev_master.process.kill()

    print '##### Starting %s master #####' % next_version
    next_master = Master(next_path, master_work_dir, credfile)
    if next_master.sleep(0.5) != True:
        print '%s master exited prematurely' % next_version
        sys.exit(1)

    print '##### Starting %s agent #####' % prev_version
    prev_agent = Agent(prev_path, agent_work_dir, credfile)
    if prev_agent.sleep(0.5) != True:
        print '%s agent exited prematurely' % prev_version
        sys.exit(1)

    print '##### Starting %s framework #####' % prev_version
    print 'Waiting for %s framework to complete (10 sec max)...' % prev_version
    prev_framework = Framework(prev_path)
    if prev_framework.sleep(10) != 0:
        print '%s framework failed with %s master and %s agent' % (prev_version,
                                                                   next_version,
                                                                   prev_version)
        sys.exit(1)


    print ''
    print 'Test case 3 (Upgrade agent)'

    print '##### Stopping %s agent #####' % prev_version
    prev_agent.process.kill()

    print '##### Starting %s agent #####' % next_version
    next_agent = Agent(next_path, agent_work_dir, credfile)
    if next_agent.sleep(0.5) != True:
        print '%s agent exited prematurely' % next_version
        sys.exit(1)

    print '##### Starting %s framework #####' % prev_version
    print 'Waiting for %s framework to complete (10 sec max)...' % prev_version
    prev_framework = Framework(prev_path)
    if prev_framework.sleep(10) != 0:
        print '%s framework failed with %s master and %s agent' % (prev_version,
                                                                   next_version,
                                                                   next_version)
        sys.exit(1)


    print ''
    print 'Test case 4 (Run of next setup)'

    print '##### Starting %s framework #####' % next_version
    print 'Waiting for %s framework to complete (10 sec max)...' % next_version
    next_framework = Framework(next_path)
    if next_framework.sleep(10) != 0:
        print '%s framework failed with %s master and %s agent' % (next_version,
                                                                   next_version,
                                                                   next_version)
        sys.exit(1)

    # Test passed.
    sys.exit(0)
