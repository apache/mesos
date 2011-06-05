%import os
%from slave import get_slave
%from webui_lib import *
%slave = get_slave()
<html>
<head>
<title>Mesos Slave on {{HOSTNAME}}</title>
<link rel="stylesheet" type="text/css" href="/static/stylesheet.css" />
</head>
<body>

<h1>Mesos Slave on {{HOSTNAME}}</h1>

<p>
Built: {{slave.build_date}} by {{slave.build_user}}<br />
Started: {{format_time(start_time)}}<br />
PID: {{slave.pid}}<br />
CPUs: {{slave.cpus}}<br />
MEM: {{format_mem(slave.mem)}}<br />
%if slave.id == -1:
  Slave ID: unassigned (not yet registered)<br />
%else:
  Slave ID: {{slave.id}}<br />
%end
Frameworks: {{slave.frameworks.size()}}<br />
</p>

<p>
Log:
<a href="/log/INFO/100">[last 100 lines]</a>
<a href="/log/INFO">[full]</a>
</p>

<h2>Frameworks</h2>

%# TODO: Sort these by framework ID
%if slave.frameworks.size() > 0:
  <table>
  <tr>
  <th>ID</th>
  <th>Name</th>
  <th>Executor</th>
  <th>Running Tasks</th>
  <th>CPUs</th>
  <th>MEM</th>
  <th>Executor Status</th>
  <th>Logs</th>
  </tr>
  %for i in range(slave.frameworks.size()):
    %framework = slave.frameworks[i]
    <tr>
    <td>{{framework.id}}</td>
    <td><a href="/framework/{{framework.id}}">{{framework.name}}</a></td>
    <td>{{os.path.basename(framework.executor_uri)}}</td>
    <td>{{framework.tasks.size()}}</td>
    <td>{{framework.cpus}}</td>
    <td>{{format_mem(framework.mem)}}</td>
    <td>{{framework.executor_status}}</td>
    <td><a href="/framework-logs/{{framework.id}}/stdout">[stdout]</a>
        <a href="/framework-logs/{{framework.id}}/stderr">[stderr]</a></td>
    </tr>
  %end
  </table>
%else:
  <p>No frameworks are active on this slave.</p>
%end

</body>
</html>
