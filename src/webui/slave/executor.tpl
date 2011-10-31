% import json
% import urllib

% from webui_lib import *

% url = "http://localhost:" + slave_port + "/slave/state.json"
% data = urllib.urlopen(url).read()
% state = json.loads(data)

<html>
<head>
  <title>Executor {{framework_id}}:{{executor_id}}</title>
  <link rel="stylesheet" type="text/css" href="/static/stylesheet.css" />
</head>
<body>

<h1>Executor {{framework_id}}:{{executor_id}}</h1>

<p>Logs:
  <a href="/executor-logs/{{framework_id}}/{{executor_id}}/stdout">[stdout]</a>
  <a href="/executor-logs/{{framework_id}}/{{executor_id}}/stderr">[stderr]</a>
</p>


% # Find the framework with the given ID.
% framework = None
% for i in range(len(state['frameworks'])):
%   if state['frameworks'][i]['id'] == framework_id:
%     framework = state['frameworks'][i]
%   end
% end

<h2> Tasks </h2>
% if framework != None:
%   for executor in framework['executors']:
%     if executor['id'] == executor_id:
        <table class="lists">
          <tr>
          <th class="lists">ID</th>
          <th class="lists">Name</th>
          <th class="lists">State</th>
          </tr>
%       for task in executor['tasks']:
          <tr>
          <td class="lists">{{task['id']}}</td>
          <td class="lists">{{task['name']}}</td>
          <td class="lists">{{task['state']}}</td>
          </tr>
%       end
        </table>
%     end
%   end
% else:
<p>No framework with ID {{framework_id}} is active on this slave.</p>
% end

<p><a href="/">Back to Slave</a></p>

</body>
</html>
