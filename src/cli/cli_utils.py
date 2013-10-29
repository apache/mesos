
# this set of utility functions are extracted from mesos-ps
# which are to be used in CLI commands such as mesos-ps, mesos-status, mesos-upload

# TODO: 
#     to be cleaned up further, e.g., sys.exit should perhaps not be used

import subprocess
import sys


# Helper that uses 'mesos-resolve' to resolve the master's IP:port.
def resolve(master):
  process = subprocess.Popen(
    ['mesos-resolve', master],
    stdin=None,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    shell=False)

  status = process.wait()
  if status != 0:
    print 'Failed to execute \'mesos-resolve %s\':\n' % master
    print process.stderr.read()
    sys.exit(1)

  result = process.stdout.read()
  process.stdout.close()
  process.stderr.close()
  return result


# Helper to determine the number of open file descriptors specified by
# ulimit (since resource.RLIMIT_NOFILE is not always accurate).
def ulimit(args):
  command = args if isinstance(args, list) else [ args ]
  command.insert(0, 'ulimit')
  process = subprocess.Popen(
    command,
    stdin=None,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    shell=False)

  status = process.wait()
  if status != 0:
    print 'Failed to execute \'ulimit %s\':\n' % ' '.join(command)
    print process.stderr.read()
    sys.exit(1)

  result = process.stdout.read()
  process.stdout.close()
  process.stderr.close()
  return result
    

