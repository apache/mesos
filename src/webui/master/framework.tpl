% import json
% import urllib

% from datetime import datetime
% from webui_lib import *

% url = "http://localhost:" + master_port + "/master/state.json"
% data = urllib.urlopen(url).read()
% state = json.loads(data)

<html>
<head>
  <title>Framework {{framework_id}}</title>
  <link rel="stylesheet" type="text/css" href="/static/stylesheet.css" />
</head>
<body>

<h1>Framework {{framework_id}}</h1>

% # Find the framework with the requested ID
% framework = None
% for i in range(len(state['frameworks'])):
%   if state['frameworks'][i]['id'] == framework_id:
%     framework = state['frameworks'][i]
%   end
% end

% if framework == None:
%   for i in range(len(state['completed_frameworks'])):
%     if state['completed_frameworks'][i]['id'] == framework_id:
%       framework = state['completed_frameworks'][i]
%     end
%   end
% end

% # Build a dict from slave ID to slave for quick lookups of slaves.
% slaves = {}
% for i in range(len(state['slaves'])):
%   slave = state['slaves'][i]
%   slaves[slave['id']] = slave
% end

% if framework != None:
<p>
  Name: {{framework['name']}}<br />
  User: {{framework['user']}}<br />
  Connected: {{format_time(framework['registered_time'])}}<br />
  Executor: {{framework['executor_uri']}}<br />
  Running Tasks: {{len(framework['tasks'])}}<br />
  CPUs: {{framework['resources']['cpus']}}<br />
  MEM: {{format_mem(framework['resources']['mem'])}}<br />
</p>

<h2>Running Tasks</h2>

% # TODO: Sort these by task ID.
% if len(framework['tasks']) > 0:
<table class="lists">
  <tr>
    <th class="lists">ID</th>
    <th class="lists">Name</th>
    <th class="lists">State</th>
    <th class="lists">Running On Slave</th>
  </tr>
  % for i in range(len(framework['tasks'])):
  %   task = framework['tasks'][i]
  <tr>
    <td class="lists">{{task['id']}}</td>
    <td class="lists">{{task['name']}}</td>
    <td class="lists">{{task['state']}}</td>
    % if task['slave_id'] in slaves:
    %   slave = slaves[task['slave_id']]
    <td class="lists"><a href="http://{{slave['web_ui_url']}}:8081/">{{slave['hostname']}}</a></td>
    % else:
    <td class="lists">Slave {{task['slave_id']}} (disconnected)</td>
    % end
  </tr>
  % end
</table>
% else:
<p>No tasks are running.</p>
% end

<h2>Completed Tasks</h2>

% if len(framework['completed_tasks']) > 0:
<table class="lists">
  <tr>
    <th class="lists">ID</th>
    <th class="lists">Name</th>
    <th class="lists">State</th>
    <th class="lists">Ran On Slave</th>
  </tr>
  % for i in range(len(framework['completed_tasks'])):
  %   task = framework['completed_tasks'][i]
  <tr>
    <td class="lists">{{task['id']}}</td>
    <td class="lists">{{task['name']}}</td>
    <td class="lists">{{task['state']}}</td>
    % if task['slave_id'] in slaves:
    %   slave = slaves[task['slave_id']]
    <td class="lists"><a href="http://{{slave['web_ui_url']}}:8081/">{{slave['hostname']}}</a></td>
    % else:
    <td class="lists">Slave {{task['slave_id']}} (disconnected)</td>
    % end
  </tr>
  % end
</table>
% else:
<p>No tasks are running.</p>
% end
% else:
<p>No framework with ID {{framework_id}} is connected.</p>
% end

<p><a href="/">Back to Master</a></p>

</body>
</html>
