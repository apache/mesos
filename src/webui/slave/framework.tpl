%from slave import get_slave
%from webui_lib import *
%slave = get_slave()
<html>
<head>
<title>Framework {{framework_id}} on Slave {{HOSTNAME}}</title>
<link rel="stylesheet" type="text/css" href="/static/stylesheet.css" />
</head>
<body>

<h1>Framework {{framework_id}} on Slave {{HOSTNAME}}</h1>

%# Find the framework with the given ID
%framework = None
%for i in range(slave.frameworks.size()):
%  if slave.frameworks[i].id == framework_id:
%    framework = slave.frameworks[i]
%  end
%end

%if framework != None:
  <p>
  Name: {{framework.name}}<br />
  Executor: {{framework.executor_uri}}<br />
  Executor Status: {{framework.executor_status}}<br />
  Running Tasks: {{framework.tasks.size()}}<br />
  CPUs: {{framework.cpus}}<br />
  MEM: {{format_mem(framework.mem)}}<br />
  </p>

  <p>Logs:
     <a href="/framework-logs/{{framework.id}}/stdout">[stdout]</a>
     <a href="/framework-logs/{{framework.id}}/stderr">[stderr]</a>
  </p>

  <h2>Tasks</h2>

  %# TODO: sort these by task id
  %if framework.tasks.size() > 0:
    <table>
    <tr>
    <th>ID</th>
    <th>Name</th>
    <th>State</th>
    </tr>
    %for i in range(framework.tasks.size()):
      %task = framework.tasks[i]
      <tr>
      <td>{{task.id}}</td>
      <td>{{task.name}}</td>
      <td>{{TASK_STATES[task.state]}}</td>
      </tr>
    %end
    </table>
  %else:
    <p>No tasks are running.</p>
  %end
%else:
  <p>No framework with ID {{framework_id}} is active on this slave.</p>
%end

<p><a href="/">Back to Slave</a></p>

</body>
</html>
