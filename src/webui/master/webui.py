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

import bottle
import commands
import os
import sys

from bottle import route, send_file, template

from optparse import OptionParser


# Parse options for this script.
parser = OptionParser()

parser.add_option('--master_port', default=None)
parser.add_option('--webui_port', default=None)
parser.add_option('--log_dir', default=None)

(options, args) = parser.parse_args(sys.argv)

assert options.master_port is not None, "Missing --master_port argument"
assert options.webui_port is not None, "Missing --webui_port argument"
assert options.log_dir is not None, "Missing --log_dir argument"

master_port = options.master_port
webui_port = options.webui_port
log_dir = options.log_dir

# Recover the webui directory from argv[0].
webui_dir = os.path.abspath(os.path.dirname(sys.argv[0]) + '/..')


@route('/')
def index():
  bottle.TEMPLATES.clear() # For rapid development
  return template("index", master_port = master_port)


@route('/framework/:id#[0-9-]*#')
def framework(id):
  bottle.TEMPLATES.clear() # For rapid development
  return template("framework", master_port = master_port, framework_id = id)


@route('/static/:filename#.*#')
def static(filename):
  send_file(filename, root = webui_dir + '/static')


@route('/log/:level#[A-Z]*#')
def log_full(level):
  send_file('mesos-master.' + level, root = log_dir,
            guessmime = False, mimetype = 'text/plain')


@route('/log/:level#[A-Z]*#/:lines#[0-9]*#')
def log_tail(level, lines):
  bottle.response.content_type = 'text/plain'
  command = 'tail -%s %s/mesos-master.%s' % (lines, log_dir, level)
  return commands.getoutput(command)


bottle.TEMPLATE_PATH.append(webui_dir + '/master')
bottle.debug(True)
bottle.run(host = '0.0.0.0', port = webui_port)
