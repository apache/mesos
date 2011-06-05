%from datetime import datetime
%from master import get_master
%from webui_lib import *
%
%master = get_master()
<html>
<head>
<title>Framework {{framework_id}} on {{HOSTNAME}}</title>
<link rel="stylesheet" type="text/css" href="/static/stylesheet.css" />
</head>
<body>

<h1>Framework {{framework_id}} on {{HOSTNAME}}</h1>

%# Find the framework with the requested ID
%framework = None
%for i in range(master.frameworks.size()):
%  if master.frameworks[i].id == framework_id:
%    framework = master.frameworks[i]
%  end
%end

%# Build a dict from slave ID to slave for quick lookups of slaves
%slaves = {}
%for i in range(master.slaves.size()):
%  slave = master.slaves[i]
%  slaves[slave.id] = slave
%end

%if framework != None:
  <p>
  Name: {{framework.name}}<br />
  Connected: {{format_time(framework.connect_time)}}<br />
  Executor: {{framework.executor}}<br />
  Running Tasks: {{framework.tasks.size()}}<br />
  CPUs: {{framework.cpus}}<br />
  MEM: {{format_mem(framework.mem)}}<br />
  </p>

  <h2>Tasks</h2>

  %# TODO: sort these by task id
  %if framework.tasks.size() > 0:
    <table>
    <tr>
    <th>ID</th>
    <th>Name</th>
    <th>State</th>
    <th>Running On</th>
    </tr>
    %for i in range(framework.tasks.size()):
      %task = framework.tasks[i]
      <tr>
      <td>{{task.id}}</td>
      <td>{{task.name}}</td>
      <td>{{TASK_STATES[task.state]}}</td>
      %if task.slave_id in slaves:
        %s = slaves[task.slave_id]
        <td><a href="http://{{s.public_dns}}:8081/">{{s.public_dns}}</a></td>
      %else:
        <td>Slave {{task.slave_id}} (disconnected)</td>
      %end
      </tr>
    %end
    </table>
  %else:
    <p>No tasks are running.</p>
  %end
%else:
  <p>No framework with ID {{framework_id}} is connected.</p>
%end

<p><a href="/">Back to Master</a></p>

</body>
</html>
