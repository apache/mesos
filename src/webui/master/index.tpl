% import json
% import os
% import time
% import urllib

% from datetime import datetime
% from webui_lib import *

% url = "http://localhost:" + master_port + "/master/state.json"
% data = urllib.urlopen(url).read()
% state = json.loads(data)

<html>
<head>
  <title>Mesos Master</title>
  <link rel="stylesheet" type="text/css" href="/static/stylesheet.css" />
</head>
<body>

<!-- TODO(benh): Actually use this table below. -->

<!-- <table class="masters" cellpadding="2" cellspacing="0" border="0" width="100%"> -->
<!--   <tr> -->
<!--     <td width="100px">Masters =></td> -->
<!--     <td class="masters-first"> -->
<!--       <a href="#"> -->
<!--         <p class="center ellipsis">smf1-ajb-35-sr1.prod.twitter.com</p> -->
<!--       </a> -->
<!--     </td> -->
<!--     <td class="masters-middle"> -->
<!--       <a href="#"> -->
<!--         <p class="center ellipsis">smf1-aex-12-sr1.prod.twitter.com</p> -->
<!--       </a> -->
<!--     </td> -->
<!--     <td class="masters-right"> -->
<!--       <a href="#"> -->
<!--         <p class="center ellipsis">smf1-ace-18-sr1.prod.twitter.com</p> -->
<!--       </a> -->
<!--     </td> -->
<!--   </tr> -->
<!--   <tr> -->
<!--     <td></td> -->
<!--     <td> -->
<!--       <p class="center" style="color: 808080; font-size: 75%;">(leader)</p> -->
<!--     </td> -->
<!--     <td> -->
<!--       <p class="center" style="color: 808080; font-size: 75%">(follower)</p> -->
<!--     </td> -->
<!--     <td> -->
<!--       <p class="center" style="color: 808080; font-size: 75%">(follower)</p> -->
<!--     </td> -->
<!--   </tr> -->
<!--   <tr> -->
<!--     <td></td> -->
<!--     <td></td> -->
<!--     <td> -->
<!--       <p class="center" style="color: 808080; font-size: 75%">(you are here)</p> -->
<!--       <br /> -->
<!--       <center><img src="/static/arrow_in_circle.jpg" width="20px" /></center> -->
<!--     </td> -->
<!--     <td></td> -->
<!--   </tr> -->
<!-- </table> -->

<h1>Mesos Master</h1>

% pid = state['pid'] # master@ip:port
% _, server = pid.split("@", 1)
% ip, _ = server.split(":", 1)
% format = "%a %b %d %Y %I:%M:%S %p " + time.strftime("%Z", time.gmtime())
% start_local = datetime.fromtimestamp(state['start_time']).strftime(format)
% start_utc = datetime.utcfromtimestamp(state['start_time']).isoformat(' ')

<p>
  Server: {{HOSTNAME}} ({{ip}})<br />
  PID: {{state['pid']}}<br />
  Built: {{state['build_date']}} by {{state['build_user']}}<br />
  Started: {{start_local}} ({{start_utc}} UTC)<br />
  ID: {{state['id']}}<br />
</p>

<p>
  Log:
  <a href="/log/INFO/100">[last 100 lines]</a>
  <a href="/log/INFO">[full]</a>
</p>

<h2>Resources</h2>

% total_cpus = 0
% total_mem = 0
% for slave in state['slaves']:
%   total_cpus += slave['resources']['cpus']
%   total_mem += slave['resources']['mem']
% end

% offered_cpus = 0
% offered_mem = 0
% for framework in state['frameworks']:
%   for offer in framework['offers']:
%     offered_cpus += offer['resources']['cpus']
%     offered_mem += offer['resources']['mem']
%   end
% end

% running_cpus = 0
% running_mem = 0
% for framework in state['frameworks']:
%   running_cpus += framework['resources']['cpus']
%   running_mem += framework['resources']['mem']
% end
% running_cpus -= offered_cpus
% running_mem -= offered_mem

% idle_cpus = total_cpus - (offered_cpus + running_cpus)
% idle_mem = total_mem - (offered_mem + running_mem)

<table>
  <tr>
    <td>Total:</td>
    <td>&nbsp;</td>
    <td align="right">{{total_cpus}} CPUs</td>
    <td>&nbsp;&nbsp;</td>
    <td align="right">{{format_mem(total_mem)}} MEM</td>
  </tr>
  <tr>
    <td>Used:</td>
    <td>&nbsp;</td>
    <td align="right">{{running_cpus}} CPUs</td>
    <td>&nbsp;&nbsp;</td>
    <td align="right">{{format_mem(running_mem)}} MEM</td>
  </tr>
  <tr>
    <td>Offered:</td>
    <td>&nbsp;</td>
    <td align="right">{{offered_cpus}} CPUs</td>
    <td>&nbsp;&nbsp;</td>
    <td align="right">{{format_mem(offered_mem)}} MEM</td>
  </tr>
  <tr>
    <td>Idle:</td>
    <td>&nbsp;</td>
    <td align="right">{{idle_cpus}} CPUs</td>
    <td>&nbsp;&nbsp;</td>
    <td align="right">{{format_mem(idle_mem)}} MEM</td>
  </tr>
</table>

<h2>Active Frameworks</h2>

% # TODO: Sort these by framework ID.
% if len(state['frameworks']) > 0:
<table class="lists">
  <tr>
    <th class="lists">ID</th>
    <th class="lists">User</th>
    <th class="lists">Name</th>
    <th class="lists">Running Tasks</th>
    <th class="lists">CPUs</th>
    <th class="lists">MEM</th>
    <th class="lists">Max Share</th>
    <th class="lists">Connected</th>
  </tr>
  % for framework in state['frameworks']:
  % cpu_share = 0
  % if total_cpus > 0:
  %   cpu_share = framework['resources']['cpus'] / float(total_cpus)
  % end
  % mem_share = 0
  % if total_mem > 0:
  %   mem_share = framework['resources']['mem'] / float(total_mem)
  % end
  % max_share = max(cpu_share, mem_share)
  <tr>
    <td class="lists">{{framework['id']}}</td>
    <td class="lists">{{framework['user']}}</td>
    <td class="lists">
      <a href="/framework/{{framework['id']}}">{{framework['name']}}</a>
    </td>
    <td class="lists">{{len(framework['tasks'])}}</td>
    <td class="lists">{{framework['resources']['cpus']}}</td>
    <td class="lists">{{format_mem(framework['resources']['mem'])}}</td>
    <td class="lists">{{'%.2f' % max_share}}</td>
    <td class="lists">{{format_time(framework['registered_time'])}}</td>
  </tr>
  % end
</table>
% else:
<p>No frameworks are connected.</p>
% end

<h2>Slaves</h2>

% # TODO: Sort these by slave ID.
% if len(state['slaves']) > 0:
<table class="lists">
  <tr>
    <th class="lists">ID</th>
    <th class="lists">Hostname</th>
    <th class="lists">CPUs</th>
    <th class="lists">MEM</th>
    <th class="lists">Connected</th>
  </tr>
  % for slave in state['slaves']:
  <tr>
    <td class="lists">{{slave['id']}}</td>
    <td class="lists">
      <a href="http://{{slave['web_ui_url']}}:8081/">{{slave['hostname']}}</a>
    </td>
    <td class="lists">{{slave['resources']['cpus']}}</td>
    <td class="lists">{{format_mem(slave['resources']['mem'])}}</td>
    <td class="lists">{{format_time(slave['registered_time'])}}</td>
  </tr>
  % end
</table>
% else:
<p>No slaves are connected.</p>
% end

<h2>Resource Offers</h2>

% # TODO: Sort these by offer ID.
% if offered_cpus > 0 or offered_mem > 0:
<table class="lists">
  <tr>
    <th class="lists">Offer ID</th>
    <th class="lists">Framework ID</th>
    <th class="lists">Slave ID</th>
    <th class="lists">CPUs</th>
    <th class="lists">MEM</th>
  </tr>
  % for framework in state['frameworks']:
  %   for offer in framework['offers']:
  <tr>
    <td class="lists">{{offer['id']}}</td>
    <td class="lists">{{offer['framework_id']}}</td>
    <td class="lists">{{offer['slave_id']}}</td>
    <td class="lists">{{offer['resources']['cpus']}}</td>
    <td class="lists">{{format_mem(offer['resources']['mem'])}}</td>
  </tr>
  %   end
  % end
</table>
% else:
<p>No offers are active.</p>
% end

<h2>Framework History</h2>
% if len(state['completed_frameworks']) > 0:
<table class="lists">
  <tr>
    <th class="lists">ID</th>
    <th class="lists">User</th>
    <th class="lists">Name</th>
    <th class="lists">Connected</th>
    <th class="lists">Disconnected</th>
  </tr>
  % for framework in state['completed_frameworks']:
  <tr>
    <td class="lists">{{framework['id']}}</td>
    <td class="lists">{{framework['user']}}</td>
    <td class="lists">
      <a href="/framework/{{framework['id']}}">{{framework['name']}}</a>
    </td>
    <td class="lists">{{format_time(framework['registered_time'])}}</td>
    <td class="lists">{{format_time(framework['unregistered_time'])}}</td>
  </tr>
  % end
</table>
% else:
<p>No frameworks have completed.</p>
% end
</body>
</html>
