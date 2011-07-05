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
import bottle
import commands
import datetime

from bottle import abort, route, send_file, template
from slave import get_slave

start_time = datetime.datetime.now()


@route('/')
def index():
  bottle.TEMPLATES.clear() # For rapid development
  return template("index", start_time = start_time)


@route('/framework/:id#.*#')
def framework(id):
  bottle.TEMPLATES.clear() # For rapid development
  return template("framework", framework_id = id)


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
  return commands.getoutput('tail -%s %s/mesos-slave.%s' % (lines, log_dir, level))


@route('/framework-logs/:fid#.*#/:log_type#[a-z]*#')
def framework_log_full(fid, log_type):
  sid = get_slave().id
  if sid != -1:
    dir = '%s/slave-%s/fw-%s' % (work_dir, sid, fid)
    i = max(os.listdir(dir))
    exec_dir = '%s/slave-%s/fw-%s/%s' % (work_dir, sid, fid, i)
    send_file(log_type, root = exec_dir,
              guessmime = False, mimetype = 'text/plain')
  else:
    abort(403, 'Slave not yet registered with master')


@route('/framework-logs/:fid#.*#/:log_type#[a-z]*#/:lines#[0-9]*#')
def framework_log_tail(fid, log_type, lines):
  bottle.response.content_type = 'text/plain'
  sid = get_slave().id
  if sid != -1:
    dir = '%s/slave-%s/fw-%s' % (work_dir, sid, fid)
    i = max(os.listdir(dir))
    filename = '%s/slave-%s/fw-%s/%s/%s' % (work_dir, sid, fid, i, log_type)
    return commands.getoutput('tail -%s %s' % (lines, filename))
  else:
    abort(403, 'Slave not yet registered with master')


bottle.TEMPLATE_PATH.append('./webui/slave/')

# TODO(*): Add an assert to confirm that all the arguments we are
# expecting have been passed to us, which will give us a better error
# message when they aren't!

init_port = sys.argv[1]
log_dir = sys.argv[2]
work_dir = sys.argv[3]

bottle.debug(True)
bottle.run(host = '0.0.0.0', port = init_port)
