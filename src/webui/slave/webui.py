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
import datetime
import json
import os
import sys
import urllib

from bottle import abort, route, send_file, template


@route('/')
def index():
  bottle.TEMPLATES.clear() # For rapid development
  return template("index", slave_port = slave_port)


@route('/framework/:id#.*#')
def framework(id):
  bottle.TEMPLATES.clear() # For rapid development
  return template("framework", slave_port = slave_port, framework_id = id)


@route('/executor/:fid#.*#/:eid#.*#')
def executor(fid, eid):
  bottle.TEMPLATES.clear() # For rapid development
  return template("executor", slave_port = slave_port, framework_id = fid, executor_id = eid)


@route('/static/:filename#.*#')
def static(filename):
  send_file(filename, root = './webui/static')


@route('/log/:level#[A-Z]*#')
def log_full(level):
  send_file('mesos-slave.' + level, root = log_dir,
            guessmime = False, mimetype = 'text/plain')


@route('/log/:level#[A-Z]*#/:lines#[0-9]*#')
def log_tail(level, lines):
  bottle.response.content_type = 'text/plain'
  command = 'tail -%s %s/mesos-slave.%s' % (lines, log_dir, level)
  return commands.getoutput(command)


@route('/executor-logs/:fid#.*#/:eid#.*#/:log_type#[a-z]*#')
def framework_log_full(fid, eid, log_type):
  url = "http://localhost:" + slave_port + "/slave/state.json"
  data = urllib.urlopen(url).read()
  state = json.loads(data)
  sid = state['id']
  if sid != -1:
    dir = '%s/slaves/%s/frameworks/%s/executors/%s/runs/' % (work_dir, sid, fid, eid)
    i = max(os.listdir(dir))
    exec_dir = '%s/slaves/%s/frameworks/%s/executors/%s/runs/%s' % (work_dir, sid, fid, eid, i)
    send_file(log_type, root = exec_dir,
              guessmime = False, mimetype = 'text/plain')
  else:
    abort(403, 'Slave not yet registered with master')


@route('/executor-logs/:fid#.*#/:eid#.*#/:log_type#[a-z]*#/:lines#[0-9]*#')
def framework_log_tail(fid, eid, log_type, lines):
  bottle.response.content_type = 'text/plain'
  url = "http://localhost:" + slave_port + "/slave/state.json"
  data = urllib.urlopen(url).read()
  state = json.loads(data)
  sid = state['id']
  if sid != -1:
    dir = '%s/slaves/%s/frameworks/%s/executors/%s/runs/' % (work_dir, sid, fid, eid)
    i = max(os.listdir(dir))
    filename = '%s/slaves/%s/frameworks/%s/executors/%s/runs/%s/%s' % (work_dir, sid, fid, eid, i, log_type)
    command = 'tail -%s %s' % (lines, filename)
    return commands.getoutput(command)
  else:
    abort(403, 'Slave not yet registered with master')


bottle.TEMPLATE_PATH.append('./webui/slave/')

# TODO(*): Add an assert to confirm that all the arguments we are
# expecting have been passed to us, which will give us a better error
# message when they aren't!

slave_port = sys.argv[1]
webui_port = sys.argv[2]
log_dir = sys.argv[3]
work_dir = sys.argv[4]

bottle.debug(True)
bottle.run(host = '0.0.0.0', port = webui_port)
