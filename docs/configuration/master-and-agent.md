---
title: Apache Mesos - Master and Agent Options
layout: documentation
---

# Master and Agent Options

*These options can be supplied to both masters and agents.*

<table class="table table-striped">
  <thead>
    <tr>
      <th width="30%">
        Flag
      </th>
      <th>
        Explanation
      </th>
    </tr>
  </thead>

<tr id="advertise_ip">
  <td>
    --advertise_ip=VALUE
  </td>
  <td>
IP address advertised to reach this Mesos master/agent.
The master/agent does not bind to this IP address.
However, this IP address may be used to access this master/agent.
  </td>
</tr>

<tr id="advertise_port">
  <td>
    --advertise_port=VALUE
  </td>
  <td>
Port advertised to reach this Mesos master/agent (along with
<code>advertise_ip</code>). The master/agent does not bind using this port.
However, this port (along with <code>advertise_ip</code>) may be used to
access Mesos master/agent.
  </td>
</tr>

<tr id="authenticate_http_readonly">
  <td>
    --[no-]authenticate_http_readonly
  </td>
  <td>
If <code>true</code>, only authenticated requests for read-only HTTP endpoints
supporting authentication are allowed. If <code>false</code>, unauthenticated
requests to such HTTP endpoints are also allowed.
  </td>
</tr>

<tr id="authenticate_http_readwrite">
  <td>
    --[no-]authenticate_http_readwrite
  </td>
  <td>
If <code>true</code>, only authenticated requests for read-write HTTP endpoints
supporting authentication are allowed. If <code>false</code>, unauthenticated
requests to such HTTP endpoints are also allowed.
  </td>
</tr>

<tr id="firewall_rules">
  <td>
    --firewall_rules=VALUE
  </td>
  <td>
The value could be a JSON-formatted string of rules or a
file path containing the JSON-formatted rules used in the endpoints
firewall. Path must be of the form <code>file:///path/to/file</code>
or <code>/path/to/file</code>.
<p/>
See the <code>Firewall</code> message in <code>flags.proto</code> for the expected format.
<p/>
Example:
<pre><code>{
  "disabled_endpoints" : {
    "paths" : [
      "/files/browse",
      "/metrics/snapshot"
    ]
  }
}</code></pre>
  </td>
</tr>

<tr id="domain">
  <td>
    --domain=VALUE
  </td>
  <td>
Domain that the master or agent belongs to. Mesos currently only supports
fault domains, which identify groups of hosts with similar failure
characteristics. A fault domain consists of a region and a zone. All masters
in the same Mesos cluster must be in the same region (they can be in
different zones). Agents configured to use a different region than the
master's region will not appear in resource offers to frameworks that have
not enabled the <code>REGION_AWARE</code> capability. This value can be
specified as either a JSON-formatted string or a file path containing JSON.

See the [documentation](../fault-domains.md) for further details.

<p/>
Example:
<pre><code>{
  "fault_domain":
    {
      "region":
        {
          "name": "aws-us-east-1"
        },
      "zone":
        {
          "name": "aws-us-east-1a"
        }
    }
}</code></pre>
  </td>
</tr>

<tr id="help">
  <td>
    --[no-]help
  </td>
  <td>
Show the help message and exit. (default: false)
  </td>
</tr>

<tr id="hooks">
  <td>
    --hooks=VALUE
  </td>
  <td>
A comma-separated list of hook modules to be installed inside master/agent.
  </td>
</tr>

<tr id="hostname">
  <td>
    --hostname=VALUE
  </td>
  <td>
The hostname the agent node should report, or that the master
should advertise in ZooKeeper.
If left unset, the hostname is resolved from the IP address
that the master/agent binds to; unless the user explicitly prevents
that, using <code>--no-hostname_lookup</code>, in which case the IP itself
is used.
  </td>
</tr>

<tr id="hostname_lookup">
  <td>
    --[no-]hostname_lookup
  </td>
  <td>
Whether we should execute a lookup to find out the server's hostname,
if not explicitly set (via, e.g., <code>--hostname</code>).
True by default; if set to <code>false</code> it will cause Mesos
to use the IP address, unless the hostname is explicitly set. (default: true)
  </td>
</tr>

<tr id="http_authenticators">
  <td>
    --http_authenticators=VALUE
  </td>
  <td>
HTTP authenticator implementation to use when handling requests to
authenticated endpoints. Use the default <code>basic</code>, or load an
alternate HTTP authenticator module using <code>--modules</code>.
(default: basic, or basic and JWT if executor authentication is enabled)
  </td>
</tr>

<tr id="ip">
  <td>
    --ip=VALUE
  </td>
  <td>
IP address to listen on. This cannot be used in conjunction
with <code>--ip_discovery_command</code>.
  </td>
</tr>

<tr id="ip_discovery_command">
  <td>
    --ip_discovery_command=VALUE
  </td>
  <td>
Optional IP discovery binary: if set, it is expected to emit
the IP address which the master/agent will try to bind to.
Cannot be used in conjunction with <code>--ip</code>.
  </td>
</tr>

<tr id="modules">
  <td>
    --modules=VALUE
  </td>
  <td>
List of modules to be loaded and be available to the internal
subsystems.
<p/>
Use <code>--modules=filepath</code> to specify the list of modules via a
file containing a JSON-formatted string. <code>filepath</code> can be
of the form <code>file:///path/to/file</code> or <code>/path/to/file</code>.
<p/>
Use <code>--modules="{...}"</code> to specify the list of modules inline.
<p/>
Example:
<pre><code>{
  "libraries": [
    {
      "file": "/path/to/libfoo.so",
      "modules": [
        {
          "name": "org_apache_mesos_bar",
          "parameters": [
            {
              "key": "X",
              "value": "Y"
            }
          ]
        },
        {
          "name": "org_apache_mesos_baz"
        }
      ]
    },
    {
      "name": "qux",
      "modules": [
        {
          "name": "org_apache_mesos_norf"
        }
      ]
    }
  ]
}</code></pre>
<p/> Cannot be used in conjunction with --modules_dir.
  </td>
</tr>

<tr id="modules_dir">
  <td>
    --modules_dir=VALUE
  </td>
  <td>
Directory path of the module manifest files. The manifest files are processed in
alphabetical order. (See <code>--modules</code> for more information on module
manifest files). Cannot be used in conjunction with <code>--modules</code>.
  </td>
</tr>

<tr id="port">
  <td>
    --port=VALUE
  </td>
  <td>
Port to listen on. (master default: 5050; agent default: 5051)
  </td>
</tr>

<tr id="version">
  <td>
    --[no-]version
  </td>
  <td>
Show version and exit. (default: false)
  </td>
</tr>

<tr id="zk_session_timeout">
  <td>
    --zk_session_timeout=VALUE
  </td>
  <td>
ZooKeeper session timeout. (default: 10secs)
  </td>
</tr>

</table>

## Logging Options

*These logging options can also be supplied to both masters and agents.*
For more about logging, see the [logging documentation](../logging.md).

<table class="table table-striped">
  <thead>
    <tr>
      <th width="30%">
        Flag
      </th>
      <th>
        Explanation
      </th>
    </tr>
  </thead>

<tr id="quiet">
  <td>
    --[no-]quiet
  </td>
  <td>
Disable logging to stderr. (default: false)
  </td>
</tr>

<tr id="log_dir">
  <td>
    --log_dir=VALUE
  </td>
  <td>
Location to put log files.  By default, nothing is written to disk.
Does not affect logging to stderr.
If specified, the log file will appear in the Mesos WebUI.
<b>NOTE</b>: 3rd party log messages (e.g. ZooKeeper) are
only written to stderr!
  </td>
</tr>

<tr id="logbufsecs">
  <td>
    --logbufsecs=VALUE
  </td>
  <td>
Maximum number of seconds that logs may be buffered for.
By default, logs are flushed immediately. (default: 0)
  </td>
</tr>

<tr id="logging_level">
  <td>
    --logging_level=VALUE
  </td>
  <td>
Log message at or above this level.
Possible values: <code>INFO</code>, <code>WARNING</code>, <code>ERROR</code>.
If <code>--quiet</code> is specified, this will only affect the logs
written to <code>--log_dir</code>, if specified. (default: INFO)
  </td>
</tr>

<tr id="initialize_driver_logging">
  <td>
    --[no-]initialize_driver_logging
  </td>
  <td>
Whether the master/agent should initialize Google logging for the
scheduler and executor drivers, in the same way as described here.
The scheduler/executor drivers have separate logs and do not get
written to the master/agent logs.
<p/>
This option has no effect when using the HTTP scheduler/executor APIs.
(default: true)
  </td>
</tr>

<tr id="external_log_file">
  <td>
    --external_log_file=VALUE
  </td>
  <td>
Location of the externally managed log file.  Mesos does not write to
this file directly and merely exposes it in the WebUI and HTTP API.
This is only useful when logging to stderr in combination with an
external logging mechanism, like syslog or journald.
<p/>
This option is meaningless when specified along with <code>--quiet</code>.
<p/>
This option takes precedence over <code>--log_dir</code> in the WebUI.
However, logs will still be written to the <code>--log_dir</code> if
that option is specified.
  </td>
</tr>

</table>
