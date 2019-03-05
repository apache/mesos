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

"""
Autogenerate documentation for all process endpoints spawned by a
Mesos master and agent.
"""

import argparse
import atexit
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import urllib.error
import urllib.parse


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
    '--registry=in_memory',
    '--work_dir=/tmp/mesos'
]

# NOTE: The agent flags here ensure that this script can run inside docker.
AGENT_COMMAND = [
    'mesos-agent.sh',
    '--master=%s:%s' % (HOST_IP, MASTER_PORT),
    '--work_dir=/tmp/mesos',
    '--systemd_enable_support=false',
    '--launcher=posix'
]


# A header to add onto all generated markdown files.
MARKDOWN_HEADER = """---
title: %s
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->
"""

# A template of the title to add onto all generated markdown files.
MARKDOWN_TITLE = "Apache Mesos - HTTP Endpoints%s"

# A global timeout as well as a retry interval when hitting any http
# endpoints on the master or agent (in seconds).
RECEIVE_TIMEOUT = 600
RETRY_INTERVAL = 0.10


class Subprocess():
    """The process running using this script."""
    def __init__(self):
        self.current = None

    def cleanup(self):
        """Kill the process running once the script is done."""
        if self.current:
            self.current.kill()


# A pointer to the top level directory of the mesos project.
GIT_TOP_DIR = subprocess.check_output(
    ['git',
     'rev-parse',
     '--show-cdup']).decode(sys.stdout.encoding).strip()

with open(os.path.join(GIT_TOP_DIR, 'CHANGELOG'), 'r') as f:
    if 'mesos' not in f.readline().lower():
        print(('You must run this command from within'
               ' the Mesos source repository!'), file=sys.stderr)
        sys.exit(1)


def parse_options():
    """Parses command line options and populates the dictionary."""

    options = {}

    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawTextHelpFormatter,
        description='Generate markdown files from all installed HTTP '
                    'endpoints on a Mesos master and agent process.')

    parser.add_argument(
        '-c', '--command-path',
        metavar='COMMAND_PATH',
        default=os.path.join(GIT_TOP_DIR, "build/bin"),
        help='Path to the Mesos master and agent commands.\n'
             '(default: %(default)s)')

    parser.add_argument(
        '-o', '--output-path',
        metavar='OUTPUT_PATH',
        default=os.path.join(GIT_TOP_DIR, "docs/endpoints"),
        help='Path to the top level directory where all\n'
             'generated markdown files will be placed.\n'
             '(default: %(default)s)')

    args = parser.parse_args()

    options['command_path'] = args.command_path
    options['output_path'] = args.output_path

    return options


def get_url_until_success(url):
    """Continuously tries to open a url until it succeeds or times out."""
    time_spent = 0
    while time_spent < RECEIVE_TIMEOUT:
        try:
            helps = urllib.request.urlopen(url)
            break
        except Exception:
            time.sleep(RETRY_INTERVAL)
            time_spent += RETRY_INTERVAL

    if time_spent >= RECEIVE_TIMEOUT:
        print('Timeout attempting to hit url: %s' % (url), file=sys.stderr)
        sys.exit(1)

    return helps.read()


def get_help(ip, port):
    """
    Grabs the help strings for all endpoints at http://ip:port as a JSON object.
    """
    url = 'http://%s:%d/help?format=json' % (ip, port)
    return json.loads(get_url_until_success(url))


def generalize_endpoint_id(p_id):
    """Generalizes the id of the form e.g. process(1) to process(id)."""
    return re.sub(r'\([0-9]+\)', '(id)', p_id)


def normalize_endpoint_id(p_id):
    """Normalizes the id of the form e.g. process(id) to process."""
    return re.sub(r'\([0-9]+\)', '', p_id)


def get_endpoint_path(p_id, name):
    """
    Generates the canonical endpoint path, given id and name.

    Examples: ('process', '/')         -> '/process'
              ('process(id)', '/')     -> '/process(id)'
              ('process', '/endpoint') -> '/process/endpoint'
    """
    # Tokenize the endpoint by '/' (filtering
    # out any empty strings between '/'s)
    path_parts = [_f for _f in name.split('/') if _f]

    # Conditionally prepend the 'id' to the list of path parts.
    # Following the notion of a 'delegate' in Mesos, we want our
    # preferred endpoint paths for the delegate process to be
    # '/endpoint' instead of '/process/endpoint'. Since this script only
    # starts 1 master and 1 agent, our only delegate processes are
    # "master" and "slave(id)". If the id matches one of these, we don't
    # prepend it, otherwise we do.
    p_id = generalize_endpoint_id(p_id)
    delegates = ["master", "slave(id)"]
    if p_id not in delegates:
        path_parts = [p_id] + path_parts

    return posixpath.join('/', *path_parts)


def get_relative_md_path(p_id, name):
    """
    Generates the relative path of the generated .md file from id and name.

    This path is relative to the options['output_path'] directory.

    Examples: master/health.md
              master/maintenance/schedule.md
              registrar/registry.md
              version.md
    """
    new_id = normalize_endpoint_id(p_id)
    # Strip the leading slash
    new_name = name[1:]

    if new_name:
        return os.path.join(new_id, new_name + '.md')
    return os.path.join(new_id + '.md')


def write_markdown(path, output, title):
    """Writes 'output' to the file at 'path'."""
    print('generating: %s' % (path))

    dirname = os.path.dirname(path)
    if not os.path.exists(dirname):
        os.makedirs(dirname)

    outfile = open(path, 'w+')

    # Add our header and remove all '\n's at the end of the output if
    # there are any.
    output = (MARKDOWN_HEADER % title) + '\n' + output.rstrip()

    outfile.write(output)
    outfile.close()


def dump_index_markdown(master_help, agent_help, options):
    """
    Dumps an index for linking to the master and agent help files.

    This file is dumped into a directory rooted at
    options['output_path'].
    """

    # The output template for the HTTP endpoints index.
    # We use a helper function below to insert text into the '%s' format
    # strings contained in the "Master Endpoints" and "Agent Endpoints"
    # sections of this template.
    output = """# HTTP Endpoints #

Below is a list of HTTP endpoints available for a given Mesos process.

Depending on your configuration, some subset of these endpoints will be
available on your Mesos master or agent. Additionally, a `/help`
endpoint will be available that displays help similar to what you see
below.

** NOTE: ** If you are using Mesos 1.1 or later, we recommend using the
new [v1 Operator HTTP API](../operator-http-api.md) instead of the
unversioned REST endpoints listed below. These endpoints will be
deprecated in the future.


** NOTE: ** The documentation for these endpoints is auto-generated from
the Mesos source code. See `support/generate-endpoint-help.py`.

## Master Endpoints ##

Below are the endpoints that are available on a Mesos master. These
endpoints are reachable at the address `http://ip:port/endpoint`.

For example, `http://master.com:5050/files/browse`.

%s

## Agent Endpoints ##

Below are the endpoints that are available on a Mesos agent. These
endpoints are reachable at the address `http://ip:port/endpoint`.

For example, `http://agent.com:5051/files/browse`.

%s
"""

    def generate_links(master_or_agent_help):
        """
        Iterates over the input JSON and creates a list of links to
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
        for process in master_or_agent_help['processes']:
            p_id = process['id']
            output += '### %s ###\n' % (generalize_endpoint_id(p_id))
            for endpoint in process['endpoints']:
                name = endpoint['name']
                output += '* [%s](%s)\n' % (get_endpoint_path(p_id, name),
                                            get_relative_md_path(p_id, name))
            output += '\n'

        # Remove any trailing newlines
        return output.rstrip()

    output = output % (generate_links(master_help),
                       generate_links(agent_help))

    path = os.path.join(options['output_path'], 'index.md')
    write_markdown(path, output, MARKDOWN_TITLE % "")


def dump_markdown(master_or_agent_help, options):
    """
    Dumps JSON encoded help strings into markdown files.

    These files are dumped into a directory rooted at
    options['output_path'].
    """
    for process in master_or_agent_help['processes']:
        p_id = process['id']
        for endpoint in process['endpoints']:
            name = endpoint['name']
            text = endpoint['text']
            title = get_endpoint_path(p_id, name)

            relative_path = get_relative_md_path(p_id, name)
            path = os.path.join(options['output_path'], relative_path)
            write_markdown(path, text, MARKDOWN_TITLE % (" - " + title))


def start_master(options):
    """
    Starts the Mesos master using the specified command.

    This method returns the Popen object used to start it so it can
    be killed later on.
    """
    cmd = os.path.join('.', options['command_path'], MASTER_COMMAND[0])
    master = subprocess.Popen([cmd] + MASTER_COMMAND[1:])

    # Wait for the master to become responsive
    get_url_until_success("http://%s:%d/health" % (HOST_IP, MASTER_PORT))
    return master


def start_agent(options):
    """
    Starts the Mesos agent using the specified command.

    This method returns the Popen object used to start it so it can
    be killed later on.
    """
    cmd = os.path.join('.', options['command_path'], AGENT_COMMAND[0])
    agent = subprocess.Popen([cmd] + AGENT_COMMAND[1:])

    # Wait for the agent to become responsive.
    get_url_until_success('http://%s:%d/health' % (HOST_IP, AGENT_PORT))
    return agent


def main():
    """
    Called when the Python script is used, we do not directly write code
    after 'if __name__ == "__main__"' as we cannot set variables in that case.
    """
    # A dictionary of the command line options passed in.
    options = parse_options()

    # A pointer to the current subprocess for the master or agent.
    # This is useful for tracking the master or agent subprocesses so
    # that we can kill them if the script exits prematurely.
    subproc = Subprocess()
    atexit.register(subproc.cleanup)
    subproc.current = start_master(options)
    master_help = get_help(HOST_IP, MASTER_PORT)
    subproc.current.kill()

    subproc.current = start_agent(options)
    agent_help = get_help(HOST_IP, AGENT_PORT)
    subproc.current.kill()

    shutil.rmtree(options['output_path'], ignore_errors=True)
    os.makedirs(options['output_path'])

    dump_index_markdown(master_help, agent_help, options)
    dump_markdown(master_help, options)
    dump_markdown(agent_help, options)

if __name__ == '__main__':
    main()
