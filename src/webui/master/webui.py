import sys
import bottle
import commands
import datetime

from bottle import route, send_file, template

start_time = datetime.datetime.now()


@route('/')
def index():
  bottle.TEMPLATES.clear() # For rapid development
  return template("index", start_time = start_time)


@route('/framework/:id#[0-9]*#')
def framework(id):
  bottle.TEMPLATES.clear() # For rapid development
  return template("framework", framework_id = int(id))


@route('/static/:filename#.*#')
def static(filename):
  send_file(filename, root = './webui/static')


@route('/log/:level#[A-Z]*#')
def log_full(level):
  send_file('mesos-master.' + level, root = '/tmp',
            guessmime = False, mimetype = 'text/plain')


@route('/log/:level#[A-Z]*#/:lines#[0-9]*#')
def log_tail(level, lines):
  bottle.response.content_type = 'text/plain'
  return commands.getoutput('tail -%s /tmp/mesos-master.%s' % (lines, level))


bottle.TEMPLATE_PATH.append('./webui/master/%s.tpl')
if sys.argv[1]:
  init_port = sys.argv[1] 
else:
  init_port = 8080
bottle.run(host = '0.0.0.0', port = init_port)
