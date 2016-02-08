#!/usr/bin/env python

'''
Autogenerate documentation for all process endpoints spawned by a
Mesos master and slave.
'''

import argparse
import atexit
import errno
import json
import os
import posixpath
import re
import shlex
import shutil
import subprocess
import sys
import time
import urllib2


# The host ip and master and agent ports.
HOST_IP = "127.0.0.1"
MASTER_PORT = 5050
AGENT_PORT = 5051

# The master and agent programs to launch.
# We considered making the parameters to these commands something that
# a user could specify on the command line, but ultimately chose to
# hard code them.  Different parameters may cause different endpoints
# to become available, and we should modify this script to ensure that
# we cover all of them instead of leaving that up to the user.
MASTER_COMMAND = [
  'mesos-master.sh',
  '--ip=%s' % (HOST_IP),
  '--registry=in_memory'
]

AGENT_COMMAND = [
  'mesos-slave.sh',
  '--master=%s:%s' % (HOST_IP, MASTER_PORT)
]


# A header to add onto all generated markdown files.
MARKDOWN_HEADER = (
'<!--- This is an automatically generated file. DO NOT EDIT! --->\n'
)


# A global timeout as well as a retry interval when hitting any http
# endpoints on the master or agent (in seconds).
RECEIVE_TIMEOUT = 10
RETRY_INTERVAL = 0.10

# A dictionary of the command line options passed in.
options = {}

# A pointer to the current subprocess for the master or agent.
# This is useful for tracking the master or agent subprocesses so
# that we can kill them if the script exits prematurely.
current_subprocess = None


# A pointer to the top level directory of the mesos project.
try:
  git_top_dir = subprocess.check_output(['git', 'rev-parse', '--show-cdup']).strip()
  with open(os.path.join(git_top_dir, 'CHANGELOG'), 'r') as f:
    if 'mesos' not in f.readline().lower():
      raise
except:
  print >> sys.stderr, 'You must run this command from within the Mesos source repository!'
  sys.exit(1)


def parse_options():
  """Parses command line options and populates the dictionary."""
  parser = argparse.ArgumentParser(
      formatter_class = argparse.RawTextHelpFormatter,
      description = 'Generate markdown files from all installed HTTP '
                    'endpoints on a Mesos master and agent process.')

  parser.add_argument(
      '-c', '--command-path',
      metavar = 'COMMAND_PATH',
      default = os.path.join(git_top_dir, "build/bin"),
      help = 'Path to the Mesos master and agent commands.\n'
             '(default: %(default)s)')

  parser.add_argument(
      '-o', '--output-path',
      metavar = 'OUTPUT_PATH',
      default = os.path.join(git_top_dir, "docs/endpoints"),
      help = 'Path to the top level directory where all\n'
             'generated markdown files will be placed.\n'
             '(default: %(default)s)')

  args = parser.parse_args()

  options['command_path'] = args.command_path
  options['output_path'] = args.output_path


def get_url_until_success(url):
  """Continuously tries to open a url until it succeeds or times out."""
  time_spent = 0
  while (time_spent < RECEIVE_TIMEOUT):
    try:
      helps = urllib2.urlopen(url)
      break
    except:
      time.sleep(RETRY_INTERVAL)
      time_spent += RETRY_INTERVAL

  if (time_spent >= RECEIVE_TIMEOUT):
    print >> sys.stderr, 'Timeout attempting to hit url: %s' % (url)
    sys.exit(1)

  return helps.read()


def get_help(ip, port):
  """Grabs the help strings for all endpoints at http://ip:port as a JSON object."""
  url = 'http://%s:%d/help?format=json' % (ip, port)
  return json.loads(get_url_until_success(url))


def generalize_endpoint_id(id):
  """Generalizes the id of the form e.g. process(id) to slave(id)."""
  return re.sub('\([0-9]+\)', '(id)', id)


def normalize_endpoint_id(id):
  """Normalizes the id of the form e.g. process(id) to process."""
  return re.sub('\([0-9]+\)', '', id)


def get_endpoint_path(id, name):
  """Generates the canonical endpoint path, given id and name.

     Examples: ('process', '/')         -> '/process'
               ('process(id)', '/')     -> '/process(id)'
               ('process', '/endpoint') -> '/process/endpoint'
  """
  new_id = generalize_endpoint_id(id)
  new_name = name[1:] # Strip the leading slash
  if new_name:
    return posixpath.join('/', new_id, new_name)
  else:
    return posixpath.join('/', new_id)


def get_relative_md_path(id, name):
  """Generates the relative path of the generated .md file from id and name.

     This path is relative to the options['output_path'] directory.

     Examples: master/health.md
               master/maintenance/schedule.md
               registrar/registry.md
               version.md
  """
  new_id = normalize_endpoint_id(id)
  new_name = name[1:] # Strip the leading slash
  if new_name:
    return os.path.join(new_id, new_name + '.md')
  else:
    return os.path.join(new_id + '.md')


def write_markdown(path, output):
  """Writes 'output' to the file at 'path'."""
  print 'generating: %s' % (path)

  dirname = os.path.dirname(path)
  if not os.path.exists(dirname):
    os.makedirs(dirname)

  outfile = open(path, 'w+')

  # Add our header and remove all '\n's at the end of the output if
  # there are any.
  output = MARKDOWN_HEADER + '\n' + output.rstrip()

  outfile.write(output)
  outfile.close()


def dump_index_markdown(master_help, agent_help):
  """Dumps an index for linking to the master and agent help files.

     This file is dumped into a directory rooted at
     options['output_path'].
  """

  # The output template for the HTTP endpoints index.
  # We use a helper function below to insert text into the '%s' format
  # strings contained in the "Master Endpoints" and "Agent Endpoints"
  # sections of this template.
  output = (
"""
# HTTP Endpoints #

Below is a list of HTTP endpoints available for a given Mesos process.

Depending on your configuration, some subset of these endpoints will
be available on your Mesos master or agent. Additionally, a `/help`
endpoint will be available that displays help similar to what you see
below.

** NOTE: ** The documentation for these endpoints is auto-generated
from strings stored in the Mesos source code. See
support/generate-endpoint-help.py.

## Master Endpoints ##

Below is a set of endpoints available on a Mesos master. These
endpoints are reachable at the address http://ip:port/endpoint.

For example, http://master.com:5050/files/browse

%s

## Agent Endpoints ##

Below is a set of endpoints available on a Mesos agent. These
endpoints are reachable at the address http://ip:port/endpoint.

For example, http://agent.com:5051/files/browse

%s
"""
  )

  def generate_links(help):
    """Iterates over the input JSON and creates a list of links to
       to the markdown files generated by this script. These links
       are grouped per process, with the process's name serving as a
       header for each group. All links are relative to
       options['output_path'].

       For example:
       ### profiler ###
       * [/profiler/start] (profiler/start.md)
       * [/profiler/stop] (profiler/stop.md)

       ### version ###
       * [/version] (version.md)
    """
    output = ""
    for process in help['processes']:
      id = process['id']
      output += '### %s ###\n' % (generalize_endpoint_id(id))
      for endpoint in process['endpoints']:
        name = endpoint['name']
        output += '* [%s](%s)\n' % (get_endpoint_path(id, name),
                                    get_relative_md_path(id, name))
      output += '\n'

    # Remove any trailing newlines
    return output.rstrip()

  output = output % (generate_links(master_help),
                     generate_links(agent_help))

  path = os.path.join(options['output_path'], 'index.md')
  write_markdown(path, output)


def dump_markdown(help):
  """Dumps JSON encoded help strings into markdown files.

     These files are dumped into a directory rooted at
     options['output_path'].
  """
  for process in help['processes']:
    id = process['id']
    for endpoint in process['endpoints']:
      name = endpoint['name']
      text = endpoint['text']

      relative_path = get_relative_md_path(id, name)
      path = os.path.join(options['output_path'], relative_path)
      write_markdown(path, text)


def start_master():
  """Starts the Mesos master using the specified command.

     This method returns the Popen object used to start it so it can
     be killed later on.
  """
  cmd = os.path.join('.', options['command_path'], MASTER_COMMAND[0])
  master = subprocess.Popen([cmd] + MASTER_COMMAND[1:])

  # Wait for the master to become responsive
  get_url_until_success("http://%s:%d/health" % (HOST_IP, MASTER_PORT))
  return master


def start_agent():
  """Starts the Mesos agent using the specified command.

     This method returns the Popen object used to start it so it can
     be killed later on.
  """
  cmd = os.path.join('.', options['command_path'], AGENT_COMMAND[0])
  agent = subprocess.Popen([cmd] + AGENT_COMMAND[1:])

  # Wait for the agent to become responsive.
  get_url_until_success('http://%s:%d/health' % (HOST_IP, AGENT_PORT))
  return agent


if __name__ == '__main__':
  parse_options()

  atexit.register(lambda: current_subprocess.kill())

  current_subprocess = start_master()
  master_help = get_help(HOST_IP, MASTER_PORT)
  current_subprocess.kill()

  current_subprocess = start_agent()
  agent_help = get_help(HOST_IP, AGENT_PORT)
  current_subprocess.kill()

  shutil.rmtree(options['output_path'], ignore_errors=True)
  os.makedirs(options['output_path'])

  dump_index_markdown(master_help, agent_help)
  dump_markdown(master_help)
  dump_markdown(agent_help)
