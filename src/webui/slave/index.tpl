% import json
% import os
% import time
% import urllib

% from datetime import datetime
% from webui_lib import *

% url = "http://localhost:" + slave_port + "/slave/state.json"
% data = urllib.urlopen(url).read()
% state = json.loads(data)

<html>
<head>
  <title>Mesos Slave</title>
  <link rel="stylesheet" type="text/css" href="/static/stylesheet.css" />
</head>
<body>

<h1>Mesos Slave</h1>

% pid = state['pid'] # slave@ip:port
% _, server = pid.split("@", 1)
% ip, _ = server.split(":", 1)
% format = "%a %b %d %Y %I:%M:%S %p " + time.strftime("%Z", time.gmtime())
% start_local = datetime.fromtimestamp(state['start_time']).strftime(format)
% start_utc = datetime.utcfromtimestamp(state['start_time']).isoformat(' ')

<p>
  Server: {{HOSTNAME}} ({{ip}})<br />
  Built: {{state['build_date']}} by {{state['build_user']}}<br />
  Started: {{start_local}} ({{start_utc}} UTC) <br />
  % if state['id'] == -1:
  ID: unassigned (not yet registered)<br />
  % else:
  ID: {{state['id']}}<br />
  % end
  CPUs: {{state['resources']['cpus']}}<br />
  MEM: {{format_mem(state['resources']['mem'])}}<br />
  Frameworks: {{len(state['frameworks'])}}<br />
</p>

<p>
  Log:
  <a href="/log/INFO/100">[last 100 lines]</a>
  <a href="/log/INFO">[full]</a>
</p>

<h2>Frameworks</h2>

% # TODO: Sort these by framework ID.
% if len(state['frameworks']) > 0:
<table class="lists">
  <tr>
    <th class="lists">ID</th>
    <th class="lists">Name</th>
    <th class="lists">Running Tasks</th>
    <th class="lists">CPUs</th>
    <th class="lists">MEM</th>
  </tr>
  % for framework in state['frameworks']:
  %   # For now just sum up the tasks and resources across all executors.
  %   tasks = 0
  %   cpus = 0
  %   mem = 0
  %   for executor in framework['executors']:
  %     tasks += len(executor['tasks'])
  %     cpus += executor['resources']['cpus']
  %     mem += executor['resources']['mem']
  %   end
  <tr>
    <td class="lists">{{framework['id']}}</td>
    <td class="lists">
      <a href="/framework/{{framework['id']}}">{{framework['name']}}</a>
    </td>
    <td class="lists">{{tasks}}</td>
    <td class="lists">{{cpus}}</td>
    <td class="lists">{{format_mem(mem)}}</td>
  </tr>
  % end
</table>
% else:
<p>No frameworks are active on this slave.</p>
% end

</body>
</html>
