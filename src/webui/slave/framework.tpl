% import json
% import urllib

% from webui_lib import *

% url = "http://localhost:" + slave_port + "/slave/state.json"
% data = urllib.urlopen(url).read()
% state = json.loads(data)

<html>
<head>
  <title>Framework {{framework_id}}</title>
  <link rel="stylesheet" type="text/css" href="/static/stylesheet.css" />
</head>
<body>

<h1>Framework {{framework_id}}</h1>

% # Find the framework with the given ID.
% framework = None
% for i in range(len(state['frameworks'])):
%   if state['frameworks'][i]['id'] == framework_id:
%     framework = state['frameworks'][i]
%   end
% end

% if framework != None:
%   # Count the number of tasks and sum the resources.
%   tasks = 0
%   cpus = 0
%   mem = 0
%   for executor in framework['executors']:
%     for task in executor['tasks']:
%       cpus += task['resources']['cpus']
%       mem += task['resources']['mem']
%       tasks += 1
%     end
%   end

<p>
  Name: {{framework['name']}}<br />
  Running Tasks: {{tasks}}<br />
  CPUs: {{cpus}}<br />
  MEM: {{format_mem(mem)}}<br />
</p>

<h2> Executors </h2>

% if len(framework['executors']) > 0:
    <table class="lists">
    <tr>
    <th class="lists">ID</th>
    <th class="lists">Tasks</th>
    <th class="lists">CPUs</th>
    <th class="lists">MEM</th>
    <th class="lists">Logs</th>
    </tr>
%   for executor in framework['executors']:
    <tr>
%     tasks = 0
%     cpus = 0
%     mem = 0
%     for task in executor['tasks']:
%       cpus += task['resources']['cpus']
%       mem += task['resources']['mem']
%       tasks += 1
%     end
      <td class="lists">
        <a href="/executor/{{framework['id']}}/{{executor['id']}}">{{executor['id']}}</a>
      </td>
      <td class="lists">{{tasks}}</td>
      <td class="lists">{{cpus}}</td>
      <td class="lists">{{mem}}</td>
      <td class="lists">
        <a href="/executor-logs/{{framework['id']}}/{{executor['id']}}/stdout">[stdout]</a>
        <a href="/executor-logs/{{framework['id']}}/{{executor['id']}}/stderr">[stderr]</a>
      </td>
    </tr>
%   end
</table>
% else:
<p> No executors for this framework are active on this slave.</p>
% end
% else:
<p>No framework with ID {{framework_id}} is active on this slave.</p>
% end

<p><a href="/">Back to Slave</a></p>

</body>
</html>
