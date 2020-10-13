---
title: Apache Mesos - Agent Options
layout: documentation
---

# Agent Options

## Required Flags

<table class=".anchored table table-striped">
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
<tr id="master">
  <td>
    --master=VALUE
  </td>
  <td>
May be one of:
  <code>host:port</code>
  <code>zk://host1:port1,host2:port2,.../path</code>
  <code>zk://username:password@host1:port1,host2:port2,.../path</code>
  <code>file:///path/to/file</code> (where file contains one of the above)
  </td>
</tr>
<tr id="work_dir">
  <td>
    --work_dir=VALUE
  </td>
  <td>
Path of the agent work directory. This is where executor sandboxes
will be placed, as well as the agent's checkpointed state in case of
failover. Note that locations like <code>/tmp</code> which are cleaned
automatically are not suitable for the work directory when running in
production, since long-running agents could lose data when cleanup
occurs. (Example: <code>/var/lib/mesos/agent</code>)
  </td>
</tr>
</table>

## Optional Flags

<table class=".anchored table table-striped">
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
<tr id="acls">
  <td>
    --acls=VALUE
  </td>
  <td>
The value could be a JSON-formatted string of ACLs
or a file path containing the JSON-formatted ACLs used
for authorization. Path could be of the form <code>file:///path/to/file</code>
or <code>/path/to/file</code>.
<p/>
Note that if the <code>--authorizer</code> flag is provided with a value
other than <code>local</code>, the ACLs contents will be
ignored.
<p/>
See the ACLs protobuf in acls.proto for the expected format.
<p/>
Example:
<pre><code>{
  "get_endpoints": [
    {
      "principals": { "values": ["a"] },
      "paths": { "values": ["/flags", "/monitor/statistics"] }
    }
  ]
}</code></pre>
  </td>
</tr>

<tr id="agent_features">
  <td>
    --agent_features=VALUE
  </td>
  <td>
JSON representation of agent features to whitelist. We always require
'MULTI_ROLE', 'HIERARCHICAL_ROLE', 'RESERVATION_REFINEMENT',
'AGENT_OPERATION_FEEDBACK', 'RESOURCE_PROVIDER', 'AGENT_DRAINING', and
'TASK_RESOURCE_LIMITS'.
<p/>
Example:
<pre><code>
{
    "capabilities": [
        {"type": "MULTI_ROLE"},
        {"type": "HIERARCHICAL_ROLE"},
        {"type": "RESERVATION_REFINEMENT"},
        {"type": "AGENT_OPERATION_FEEDBACK"},
        {"type": "RESOURCE_PROVIDER"},
        {"type": "AGENT_DRAINING"},
        {"type": "TASK_RESOURCE_LIMITS"}
    ]
}
</pre></code>
  </td>
</tr>

<tr id="agent_subsystems">
  <td>
    --agent_subsystems=VALUE,
    <p/>
    --slave_subsystems=VALUE
  </td>
  <td>
List of comma-separated cgroup subsystems to run the agent binary
in, e.g., <code>memory,cpuacct</code>. The default is none.
Present functionality is intended for resource monitoring and
no cgroup limits are set, they are inherited from the root mesos
cgroup.
  </td>
</tr>

<tr id="effective_capabilities">
  <td>
    --effective_capabilities=VALUE
  </td>
  <td>
JSON representation of the Linux capabilities that the agent will
grant to a task that will be run in containers launched by the
containerizer (currently only supported by the Mesos Containerizer).
This set overrides the default capabilities for the user but not
the capabilities requested by the framework.
<p/>
To set capabilities the agent should have the <code>SETPCAP</code> capability.
<p/>
This flag is effective iff <code>linux/capabilities</code> isolation is enabled.
When <code>linux/capabilities</code> isolation is enabled, the absence of this flag
implies that the operator intends to allow ALL capabilities.
<p/>
Example:
<pre><code>
{
  "capabilities": [
    "NET_RAW",
    "SYS_ADMIN"
  ]
}
</code></pre>
  </td>
</tr>

<tr id="bounding_capabilities">
  <td>
    --bounding_capabilities=VALUE
  </td>
  <td>
JSON representation of the Linux capabilities that the operator
will allow as the maximum level of privilege that a task launched
by the containerizer may acquire (currently only supported by the
Mesos Containerizer).
<p/>
This flag is effective iff <code>linux/capabilities</code> isolation is enabled.
When <code>linux/capabilities</code> isolation is enabled, the absence of this flag
implies that the operator intends to allow ALL capabilities.
<p/>
This flag has the same syntax as <code>--effective_capabilities</code>.
  </td>
</tr>

<tr id="appc_simple_discovery_uri_prefix">
  <td>
    --appc_simple_discovery_uri_prefix=VALUE
  </td>
  <td>
URI prefix to be used for simple discovery of appc images,
e.g., <code>http://</code>, <code>https://</code>,
<code>hdfs://<hostname>:9000/user/abc/cde</code>.
(default: http://)
  </td>
</tr>

<tr id="appc_store_dir">
  <td>
    --appc_store_dir=VALUE
  </td>
  <td>
Directory the appc provisioner will store images in.
(default: /tmp/mesos/store/appc)
  </td>
</tr>

<tr id="attributes">
  <td>
    --attributes=VALUE
  </td>
  <td>
Attributes of the agent machine, in the form:
<code>rack:2</code> or <code>rack:2;u:1</code>
  </td>
</tr>

<tr id="authenticate_http_executors">
  <td>
    --[no-]authenticate_http_executors
  </td>
  <td>
If <code>true</code>, only authenticated requests for the HTTP executor API are
allowed. If <code>false</code>, unauthenticated requests are also allowed. This
flag is only available when Mesos is built with SSL support.
(default: false)
  </td>
</tr>

<tr id="authenticatee">
  <td>
    --authenticatee=VALUE
  </td>
  <td>
Authenticatee implementation to use when authenticating against the
master. Use the default <code>crammd5</code>, or
load an alternate authenticatee module using <code>--modules</code>. (default: crammd5)
  </td>
</tr>

<tr id="authentication_backoff_factor">
  <td>
    --authentication_backoff_factor=VALUE
  </td>
  <td>
The agent will time out its authentication with the master based on
exponential backoff. The timeout will be randomly chosen within the
range <code>[min, min + factor*2^n]</code> where <code>n</code> is the number
of failed attempts. To tune these parameters, set the
<code>--authentication_timeout_[min|max|factor]</code> flags. (default: 1secs)
  </td>
</tr>

<tr id="authentication_timeout_min">
  <td>
    --authentication_timeout_min=VALUE
  </td>
  <td>
The minimum amount of time the agent waits before retrying authenticating
with the master. See <code>--authentication_backoff_factor</code> for more
details. (default: 5secs)
<p/>NOTE that since authentication retry cancels the previous authentication
request, one should consider what is the normal authentication delay when
setting this flag to prevent premature retry.</p>
  </td>
</tr>

<tr id="authentication_timeout_max">
  <td>
    --authentication_timeout_max=VALUE
  </td>
  <td>
The maximum amount of time the agent waits before retrying authenticating
with the master. See <code>--authentication_backoff_factor</code> for more
details. (default: 1mins)
  </td>
</tr>

<tr id="authorizer">
  <td>
    --authorizer=VALUE
  </td>
  <td>
Authorizer implementation to use when authorizing actions that
require it.
Use the default <code>local</code>, or
load an alternate authorizer module using <code>--modules</code>.
<p/>
Note that if the <code>--authorizer</code> flag is provided with a value
other than the default <code>local</code>, the ACLs
passed through the <code>--acls</code> flag will be ignored.
  </td>
</tr>

<tr id="cgroups_cpu_enable_pids_and_tids_count">
  <td>
    --[no]-cgroups_cpu_enable_pids_and_tids_count
  </td>
  <td>
Cgroups feature flag to enable counting of processes and threads
inside a container. (default: false)
  </td>
</tr>

<tr id="cgroups_destroy_timeout">
  <td>
    --cgroups_destroy_timeout=VALUE
  </td>
  <td>
Amount of time allowed to destroy a cgroup hierarchy. If the cgroup
hierarchy is not destroyed within the timeout, the corresponding
container destroy is considered failed. (default: 1mins)
  </td>
</tr>

<tr id="cgroups_enable_cfs">
  <td>
    --[no]-cgroups_enable_cfs
  </td>
  <td>
Cgroups feature flag to enable hard limits on CPU resources
via the CFS bandwidth limiting subfeature. (default: false)
  </td>
</tr>

<tr id="cgroups_hierarchy">
  <td>
    --cgroups_hierarchy=VALUE
  </td>
  <td>
The path to the cgroups hierarchy root. (default: /sys/fs/cgroup)
  </td>
</tr>

<tr id="cgroups_limit_swap">
  <td>
    --[no]-cgroups_limit_swap
  </td>
  <td>
Cgroups feature flag to enable memory limits on both memory and
swap instead of just memory. (default: false)
  </td>
</tr>

<tr id="cgroups_net_cls_primary_handle">
  <td>
    --cgroups_net_cls_primary_handle
  </td>
  <td>
A non-zero, 16-bit handle of the form `0xAAAA`. This will be used as
the primary handle for the net_cls cgroup.
  </td>
</tr>

<tr id="cgroups_net_cls_secondary_handles">
  <td>
    --cgroups_net_cls_secondary_handles
  </td>
  <td>
A range of the form 0xAAAA,0xBBBB, specifying the valid secondary
handles that can be used with the primary handle. This will take
effect only when the <code>--cgroups_net_cls_primary_handle</code> is set.
  </td>
</tr>

<tr id="allowed_devices">
  <td>
    --allowed_devices
  </td>
  <td>
JSON object representing the devices that will be additionally
whitelisted by cgroups devices subsystem. Noted that the following
devices always be whitelisted by default:
<pre><code>  * /dev/console
  * /dev/tty0
  * /dev/tty1
  * /dev/pts/*
  * /dev/ptmx
  * /dev/net/tun
  * /dev/null
  * /dev/zero
  * /dev/full
  * /dev/tty
  * /dev/urandom
  * /dev/random
</code></pre>
This flag will take effect only when <code>cgroups/devices</code> is set in
<code>--isolation</code> flag.
<p/>
Example:
<pre><code>{
  "allowed_devices": [
    {
      "device": {
        "path": "/path/to/device"
      },
      "access": {
        "read": true,
        "write": false,
        "mknod": false
      }
    }
  ]
}
</code></pre>
  </td>
</tr>

<tr id="cgroups_root">
  <td>
    --cgroups_root=VALUE
  </td>
  <td>
Name of the root cgroup. (default: mesos)
  </td>
</tr>

<tr id="check_agent_port_range_only">
  <td>
    --[no-]check_agent_port_range_only
  </td>
  <td>
When this is true, the <code>network/ports</code> isolator allows tasks to
listen on additional ports provided they fall outside the range
published by the agent's resources. Otherwise tasks are restricted
to only listen on ports for which they have been assigned resources.
(default: false); This flag can't be used in conjunction with
<code>--container_ports_isolated_range</code>.
  </td>
</tr>

<tr id="container_disk_watch_interval">
  <td>
    --container_disk_watch_interval=VALUE
  </td>
  <td>
The interval between disk quota checks for containers. This flag is
used for the <code>disk/du</code> isolator. (default: 15secs)
  </td>
</tr>

<tr id="container_logger">
  <td>
    --container_logger=VALUE
  </td>
  <td>
The name of the container logger to use for logging container
(i.e., executor and task) stdout and stderr. The default
container logger writes to <code>stdout</code> and <code>stderr</code> files
in the sandbox directory.
  </td>
</tr>

<tr id="container_ports_isolated_range">
  <td>
    --container_ports_isolated_range=VALUE
  </td>
  <td>
When this flag is set, <code>network/ports</code> isolator will only enforce
the port isolation for the given range of ports range. This flag can't
be used in conjunction with <code>--check_agent_port_range_only</code>.
Example: <code>[0-35000]</code>
  </td>
</tr>

<tr id="container_ports_watch_interval">
  <td>
    --container_ports_watch_interval=VALUE
  </td>
  <td>
Interval at which the <code>network/ports</code> isolator should check for
containers listening on ports they don't have resources for.
(default: 30secs)
  </td>
</tr>

<tr id="containerizers">
  <td>
    --containerizers=VALUE
  </td>
  <td>
Comma-separated list of containerizer implementations
to compose in order to provide containerization.
Available options are <code>mesos</code> and
<code>docker</code> (on Linux). The order the containerizers
are specified is the order they are tried.
(default: mesos)
  </td>
</tr>

<tr id="credential">
  <td>
    --credential=VALUE
  </td>
  <td>
Path to a JSON-formatted file containing the credential
to use to authenticate with the master.
Path could be of the form <code>file:///path/to/file</code> or <code>/path/to/file</code>.
Example:
<pre><code>{
  "principal": "username",
  "secret": "secret"
}</code></pre>
  </td>
</tr>

<tr id="default_container_dns">
  <td>
    --default_container_dns=VALUE
  </td>
  <td>
JSON-formatted DNS information for CNI networks (Mesos containerizer)
and CNM networks (Docker containerizer). For CNI networks, this flag
can be used to configure `nameservers`, `domain`, `search` and
`options`, and its priority is lower than the DNS information returned
by a CNI plugin, but higher than the DNS information in agent host's
/etc/resolv.conf. For CNM networks, this flag can be used to configure
`nameservers`, `search` and `options`, it will only be used if there
is no DNS information provided in the ContainerInfo.docker.parameters
message.
<p/>
See the ContainerDNS message in `flags.proto` for the expected format.
<p/>
Example:
<pre><code>{
  "mesos": [
    {
      "network_mode": "CNI",
      "network_name": "net1",
      "dns": {
        "nameservers": [ "8.8.8.8", "8.8.4.4" ]
      }
    }
  ],
  "docker": [
    {
      "network_mode": "BRIDGE",
      "dns": {
        "nameservers": [ "8.8.8.8", "8.8.4.4" ]
      }
    },
    {
      "network_mode": "USER",
      "network_name": "net2",
      "dns": {
        "nameservers": [ "8.8.8.8", "8.8.4.4" ]
      }
    }
  ]
}</code></pre>
  </td>
</tr>

<tr id="default_container_info">
  <td>
    --default_container_info=VALUE
  </td>
  <td>
JSON-formatted ContainerInfo that will be included into
any ExecutorInfo that does not specify a ContainerInfo.
<p/>
See the ContainerInfo protobuf in mesos.proto for
the expected format.
<p/>
Example:
<pre><code>{
  "type": "MESOS",
  "volumes": [
    {
      "host_path": ".private/tmp",
      "container_path": "/tmp",
      "mode": "RW"
    }
  ]
}</code></pre>
  </td>
</tr>

<tr id="default_role">
  <td>
    --default_role=VALUE
  </td>
  <td>
Any resources in the <code>--resources</code> flag that
omit a role, as well as any resources that
are not present in <code>--resources</code> but that are
automatically detected, will be assigned to
this role. (default: *)
  </td>
</tr>

<tr id="default_container_shm_size">
  <td>
    --default_container_shm_size
  </td>
  <td>
The default size of the /dev/shm for the container which has its own
/dev/shm but does not specify the <code>shm_size</code> field in its
<code>LinuxInfo</code>. The format is [number][unit], number must be
a positive integer and unit can be B (bytes), KB (kilobytes), MB
(megabytes), GB (gigabytes) or TB (terabytes). Note that this flag is
only relevant for the Mesos Containerizer and it will be ignored if
the <code>namespaces/ipc</code> isolator is not enabled.
  </td>
</tr>

<tr id="disallow_sharing_agent_ipc_namespace">
  <td>
    --[no-]disallow_sharing_agent_ipc_namespace
  </td>
  <td>
If set to <code>true</code>, each top-level container will have its own IPC
namespace and /dev/shm, and if the framework requests to share the agent IPC
namespace and /dev/shm for the top level container, the container launch will
be rejected. If set to <code>false</code>, the top-level containers will share
the IPC namespace and /dev/shm with agent if the framework requests it. This
flag will be ignored if the <code>namespaces/ipc</code> isolator is not enabled.
(default: false)
  </td>
</tr>

<tr id="disallow_sharing_agent_pid_namespace">
  <td>
    --[no-]disallow_sharing_agent_pid_namespace
  </td>
  <td>
If set to <code>true</code>, each top-level container will have its own pid
namespace, and if the framework requests to share the agent pid namespace for
the top level container, the container launch will be rejected. If set to
<code>false</code>, the top-level containers will share the pid namespace with
agent if the framework requests it. This flag will be ignored if the <code>
namespaces/pid</code> isolator is not enabled.
(default: false)
  </td>
</tr>

<tr id="disk_profile_adaptor">
  <td>
    --disk_profile_adaptor=VALUE
  </td>
  <td>
The name of the disk profile adaptor module that storage resource
providers should use for translating a 'disk profile' into inputs
consumed by various Container Storage Interface (CSI) plugins.
If this flag is not specified, the default behavior for storage
resource providers is to only expose resources for pre-existing
volumes and not publish RAW volumes.
  </td>
</tr>

<tr id="disk_watch_interval">
  <td>
    --disk_watch_interval=VALUE
  </td>
  <td>
Periodic time interval (e.g., 10secs, 2mins, etc)
to check the overall disk usage managed by the agent.
This drives the garbage collection of archived
information and sandboxes. (default: 1mins)
  </td>
</tr>

<tr id="docker">
  <td>
    --docker=VALUE
  </td>
  <td>
The absolute path to the docker executable for docker
containerizer.
(default: docker)
  </td>
</tr>

<tr id="docker_config">
  <td>
    --docker_config=VALUE
  </td>
  <td>
The default docker config file for agent. Can be provided either as an
absolute path pointing to the agent local docker config file, or as a
JSON-formatted string. The format of the docker config file should be
identical to docker's default one (e.g., either
<code>$HOME/.docker/config.json</code> or <code>$HOME/.dockercfg</code>).
Example JSON (<code>$HOME/.docker/config.json</code>):
<pre><code>{
  "auths": {
    "https://index.docker.io/v1/": {
      "auth": "xXxXxXxXxXx=",
      "email": "username@example.com"
    }
  }
}
</code></pre>
  </td>
</tr>

<tr id="docker_ignore_runtime">
  <td>
    --docker_ignore_runtime=VALUE
  </td>
  <td>
Ignore any runtime configuration specified in the Docker image. The
Mesos containerizer will not propagate Docker runtime specifications
such as <code>WORKDIR</code>, <code>ENV</code> and <code>CMD</code>
to the container.
(default: false)
  </td>
</tr>

<tr id="docker_kill_orphans">
  <td>
    --[no-]docker_kill_orphans
  </td>
  <td>
Enable docker containerizer to kill orphaned containers.
You should consider setting this to false when you launch multiple
agents in the same OS, to avoid one of the DockerContainerizer
removing docker tasks launched by other agents.
(default: true)
  </td>
</tr>

<tr id="docker_mesos_image">
  <td>
    --docker_mesos_image=VALUE
  </td>
  <td>
The Docker image used to launch this Mesos agent instance.
If an image is specified, the docker containerizer assumes the agent
is running in a docker container, and launches executors with
docker containers in order to recover them when the agent restarts and
recovers.
  </td>
</tr>

<tr id="docker_registry">
  <td>
    --docker_registry=VALUE
  </td>
  <td>
The default url for Mesos containerizer to pull Docker images. It could
either be a Docker registry server url (e.g., <code>https://registry.docker.io</code>),
or a source that Docker image archives (result of <code>docker save</code>) are
stored. The Docker archive source could be specified either as a local
path (e.g., <code>/tmp/docker/images</code>), or as an HDFS URI (*experimental*)
(e.g., <code>hdfs://localhost:8020/archives/</code>). Note that this option won't
change the default registry server for Docker containerizer.
(default: https://registry-1.docker.io)
  </td>
</tr>

<tr id="docker_remove_delay">
  <td>
    --docker_remove_delay=VALUE
  </td>
  <td>
The amount of time to wait before removing docker containers (i.e., `docker rm`)
after Mesos regards the container as TERMINATED
(e.g., <code>3days</code>, <code>2weeks</code>, etc).
This only applies for the Docker Containerizer. (default: 6hrs)
  </td>
</tr>

<tr id="docker_socket">
  <td>
    --docker_socket=VALUE
  </td>
  <td>
Resource used by the agent and the executor to provide CLI access to the
Docker daemon. On Unix, this is typically a path to a socket, such as
<code>/var/run/docker.sock</code>. On Windows this must be a named pipe,
such as <code>//./pipe/docker_engine</code>. <b>NOTE</b>: This must be the path
used by the Docker image used to run the agent. (default:
//./pipe/docker_engine on Windows; /var/run/docker.sock on other
platforms).
  </td>
</tr>

<tr id="docker_stop_timeout">
  <td>
    --docker_stop_timeout=VALUE
  </td>
  <td>
The time docker daemon waits after stopping a container before killing
that container. This flag is deprecated; use task's kill policy instead.
(default: 0ns)
  </td>
</tr>

<tr id="docker_store_dir">
  <td>
    --docker_store_dir=VALUE
  </td>
  <td>
Directory the Docker provisioner will store images in (default: /tmp/mesos/store/docker)
  </td>
</tr>

<tr id="docker_volume_checkpoint_dir">
  <td>
    --docker_volume_checkpoint_dir=VALUE
  </td>
  <td>
The root directory where we checkpoint the information about docker
volumes that each container uses.
(default: /var/run/mesos/isolators/docker/volume)
  </td>
</tr>

<tr id="docker_volume_chown">
  <td>
    --[no-]docker_volume_chown
  </td>
  <td>
Whether to chown the docker volume's mount point non-recursively
to the container user. Please notice that this flag is not recommended
to turn on if there is any docker volume shared by multiple non-root
users. By default, this flag is off. (default: false)
  </td>
</tr>

<tr id="domain_socket_location">
  <td>
    --domain_socket_location=VALUE
  </td>
  <td>
Location on the host filesystem of the domain socket used for
communication with executors. Alternatively, this can be set to
<code>'systemd:&lt;identifier&gt;'</code> to use the domain socket
with the given identifier, which is expected to be passed by systemd.

This flag will be ignored unless the <code>--http_executor_domain_sockets</code>
flag is also set to true.

Total path length must be less than 108 characters.

Will be set to <code>&lt;runtime_dir&gt;/agent.sock</code> by default.
  </td>
</tr>

<tr id="enforce_container_disk_quota">
  <td>
    --[no-]enforce_container_disk_quota
  </td>
  <td>
Whether to enable disk quota enforcement for containers. This flag
is used by the <code>disk/du</code> and <code>disk/xfs</code> isolators. (default: false)
  </td>
</tr>

<tr id="enforce_container_ports">
  <td>
    --[no-]enforce_container_ports
  </td>
  <td>
Whether to enable network port enforcement for containers. This flag
is used by the <code>network/ports</code> isolator. (default: false)
  </td>
</tr>

<tr id="executor_environment_variables">
  <td>
    --executor_environment_variables=VALUE
  </td>
  <td>
JSON object representing the environment variables that should be
passed to the executor, and thus subsequently task(s). By default this
flag is none. Users have to define executor environment explicitly.
Example:
<pre><code>{
  "PATH": "/bin:/usr/bin",
  "LD_LIBRARY_PATH": "/usr/local/lib"
}</code></pre>
  </td>
</tr>

<tr id="executor_registration_timeout">
  <td>
    --executor_registration_timeout=VALUE
  </td>
  <td>
Amount of time to wait for an executor
to register with the agent before considering it hung and
shutting it down (e.g., 60secs, 3mins, etc) (default: 1mins)
  </td>
</tr>

<tr id="executor_reregistration_timeout">
  <td>
    --executor_reregistration_timeout=VALUE
  </td>
  <td>
The timeout within which an executor is expected to reregister after
the agent has restarted, before the agent considers it gone and shuts
it down. Note that currently, the agent will not reregister with the
master until this timeout has elapsed (see MESOS-7539). (default: 2secs)
  </td>
</tr>

<tr id="executor_reregistration_retry_interval">
  <td>
    --executor_reregistration_retry_interval=VALUE
  </td>
  <td>
For PID-based executors, how long the agent waits before retrying
the reconnect message sent to the executor during recovery.
NOTE: Do not use this unless you understand the following
(see MESOS-5332): PID-based executors using Mesos libraries &gt;= 1.1.2
always re-link with the agent upon receiving the reconnect message.
This avoids the executor replying on a half-open TCP connection to
the old agent (possible if netfilter is dropping packets,
see: MESOS-7057). However, PID-based executors using Mesos
libraries &lt; 1.1.2 do not re-link and are therefore prone to
replying on a half-open connection after the agent restarts. If we
only send a single reconnect message, these "old" executors will
reply on their half-open connection and receive a RST; without any
retries, they will fail to reconnect and be killed by the agent once
the executor re-registration timeout elapses. To ensure these "old"
executors can reconnect in the presence of netfilter dropping
packets, we introduced optional retries of the reconnect message.
This results in "old" executors correctly establishing a link
when processing the second reconnect message. (default: no retries)
  </td>
</tr>

<tr id="max_completed_executors_per_framework">
  <td>
    --max_completed_executors_per_framework=VALUE
  </td>
  <td>
Maximum number of completed executors per framework to store
in memory. (default: 150)
  </td>
</tr>

<tr id="jwt_secret_key">
  <td>
    --jwt_secret_key=VALUE
  </td>
  <td>
Path to a file containing the key used when generating JWT secrets.
This flag is only available when Mesos is built with SSL support.
  </td>
</tr>

<tr id="executor_shutdown_grace_period">
  <td>
    --executor_shutdown_grace_period=VALUE
  </td>
  <td>
Default amount of time to wait for an executor to shut down
(e.g. 60secs, 3mins, etc). ExecutorInfo.shutdown_grace_period
overrides this default. Note that the executor must not assume
that it will always be allotted the full grace period, as the
agent may decide to allot a shorter period, and failures / forcible
terminations may occur.
(default: 5secs)
  </td>
</tr>

<tr id="fetcher_cache_dir">
  <td>
    --fetcher_cache_dir=VALUE
  </td>
  <td>
Parent directory for fetcher cache directories
(one subdirectory per agent). (default: /tmp/mesos/fetch)

Directory for the fetcher cache. The agent will clear this directory
on startup. It is recommended to set this value to a separate volume
for several reasons:
<ul>
<li> The cache directories are transient and not meant to be
     backed up. Upon restarting the agent, the cache is always empty. </li>
<li> The cache and container sandboxes can potentially interfere with
     each other when occupying a shared space (i.e. disk contention). </li>
</ul>
  </td>
</tr>

<tr id="fetcher_cache_size">
  <td>
    --fetcher_cache_size=VALUE
  </td>
  <td>
Size of the fetcher cache in Bytes. (default: 2GB)
  </td>
</tr>

<tr id="fetcher_stall_timeout">
  <td>
    --fetcher_stall_timeout=VALUE
  </td>
  <td>
Amount of time for the fetcher to wait before considering a download
being too slow and abort it when the download stalls (i.e., the speed
keeps below one byte per second).
<b>NOTE</b>: This feature only applies when downloading data from the net and
does not apply to HDFS. (default: 1mins)
  </td>
</tr>

<tr id="frameworks_home">
  <td>
    --frameworks_home=VALUE
  </td>
  <td>
Directory path prepended to relative executor URIs (default: )
  </td>
</tr>

<tr id="gc_delay">
  <td>
    --gc_delay=VALUE
  </td>
  <td>
Maximum amount of time to wait before cleaning up
executor directories (e.g., 3days, 2weeks, etc).
Note that this delay may be shorter depending on
the available disk usage. (default: 1weeks)
  </td>
</tr>

<tr id="gc_disk_headroom">
  <td>
    --gc_disk_headroom=VALUE
  </td>
  <td>
Adjust disk headroom used to calculate maximum executor
directory age. Age is calculated by:
<code>gc_delay * max(0.0, (1.0 - gc_disk_headroom - disk usage))</code>
every <code>--disk_watch_interval</code> duration. <code>gc_disk_headroom</code> must
be a value between 0.0 and 1.0 (default: 0.1)
  </td>
</tr>

<tr id="gc_non_executor_container_sandboxes">
  <td>
    --[no-]gc_non_executor_container_sandboxes
  </td>
  <td>
Determines whether nested container sandboxes created via the
<code>LAUNCH_CONTAINER</code> and <code>LAUNCH_NESTED_CONTAINER</code> APIs will be
automatically garbage collected by the agent upon termination.
The <code>REMOVE_(NESTED_)CONTAINER</code> API is unaffected by this flag
and can still be used. (default: false).
  </td>
</tr>

<tr id="hadoop_home">
  <td>
    --hadoop_home=VALUE
  </td>
  <td>
Path to find Hadoop installed (for
fetching framework executors from HDFS)
(no default, look for <code>HADOOP_HOME</code> in
environment or find hadoop on <code>PATH</code>)
  </td>
</tr>

<tr id="host_path_volume_force_creation">
  <td>
    --host_path_volume_force_creation
  </td>
  <td>
A colon-separated list of directories where descendant directories are
allowed to be created by the <code>volume/host_path</code> isolator,
if the directories do not exist.
  </td>
</tr>

<tr id="http_credentials">
  <td>
    --http_credentials=VALUE
  </td>
  <td>
Path to a JSON-formatted file containing credentials. These
credentials are used to authenticate HTTP endpoints on the agent.
Path can be of the form <code>file:///path/to/file</code> or <code>/path/to/file</code>.
<p/>
Example:
<pre><code>{
  "credentials": [
    {
      "principal": "yoda",
      "secret": "usetheforce"
    }
  ]
}
</code></pre>
  </td>
</tr>

<tr id="http_command_executor">
  <td>
    --[no-]http_command_executor
  </td>
  <td>
The underlying executor library to be used for the command executor.
If set to <code>true</code>, the command executor would use the HTTP based
executor library to interact with the Mesos agent. If set to <code>false</code>,
the driver based implementation would be used.
<b>NOTE</b>: This flag is *experimental* and should not be used in
production yet. (default: false)
  </td>
</tr>


<tr id="http_executor_domain_sockets">
  <td>
      --http_executor_domain_sockets
  </td>
  <td>
If true, the agent will provide a unix domain sockets that the
executor can use to connect to the agent, instead of relying on
a TCP connection.
  </td>
</tr>

<tr id="http_heartbeat_interval">
  <td>
    --http_heartbeat_interval=VALUE
  </td>
  <td>
This flag sets a heartbeat interval (e.g. '5secs', '10mins') for
messages to be sent over persistent connections made against
the agent HTTP API. Currently, this only applies to the
<code>LAUNCH_NESTED_CONTAINER_SESSION</code> and <code>ATTACH_CONTAINER_OUTPUT</code> calls.
(default: 30secs)
  </td>
</tr>

<tr id="image_providers">
  <td>
    --image_providers=VALUE
  </td>
  <td>
Comma-separated list of supported image providers,
e.g., <code>APPC,DOCKER</code>.
  </td>
</tr>

<tr id="image_provisioner_backend">
  <td>
    --image_provisioner_backend=VALUE
  </td>
  <td>
Strategy for provisioning container rootfs from images, e.g., <code>aufs</code>,
<code>bind</code>, <code>copy</code>, <code>overlay</code>.
  </td>
</tr>

<tr id="image_gc_config">
  <td>
    --image_gc_config=VALUE
  </td>
  <td>
JSON-formatted configuration for automatic container image garbage
collection. This is an optional flag. If it is not set, it means
the automatic container image gc is not enabled. Users have to
trigger image gc manually via the operator API. If it is set, the
auto image gc is enabled. This image gc config can be provided either
as a path pointing to a local file, or as a JSON-formatted string.
Please note that the image garbage collection only work with Mesos
Containerizer for now.
<p/>
See the ImageGcConfig message in `flags.proto` for the expected
format.
<p/>
In the following example, image garbage collection is configured to
sample disk usage every hour, and will attempt to maintain at least
10% of free space on the container image filesystem:
<pre><code>{
  "image_disk_headroom": 0.1,
  "image_disk_watch_interval": {
    "nanoseconds": 3600000000000
  },
  "excluded_images": []
}</code></pre>
  </td>
</tr>

<tr id="ip6">
  <td>
    --ip6=VALUE
  </td>
  <td>
IPv6 address to listen on. This cannot be used in conjunction
with <code>--ip6_discovery_command</code>.
<p/>
NOTE: Currently Mesos doesn't listen on IPv6 sockets and hence
this IPv6 address is only used to advertise IPv6 addresses for
containers running on the host network.
  </td>
</tr>

<tr id="ip6_discovery_command">
  <td>
    --ip6_discovery_command=VALUE
  </td>
  <td>
Optional IPv6 discovery binary: if set, it is expected to emit
the IPv6 address on which Mesos will try to bind when IPv6 socket
support is enabled in Mesos.
<p/>
NOTE: Currently Mesos doesn't listen on IPv6 sockets and hence
this IPv6 address is only used to advertise IPv6 addresses for
containers running on the host network.
  </td>
</tr>

<tr id="isolation">
  <td>
    --isolation=VALUE
  </td>
  <td>
Isolation mechanisms to use, e.g., <code>posix/cpu,posix/mem</code> (or
<code>windows/cpu,windows/mem</code> if you are on Windows), or
<code>cgroups/cpu,cgroups/mem</code>, or <code>network/port_mapping</code>
(configure with flag: <code>--with-network-isolator</code> to enable),
or <code>gpu/nvidia</code> for nvidia specific gpu isolation, or load an alternate
isolator module using the <code>--modules</code> flag. If <code>cgroups/all</code>
is specified, any other cgroups related isolation options (e.g.,
<code>cgroups/cpu</code>) will be ignored, and all the local enabled cgroups
subsystems on the agent host will be automatically loaded by the cgroups isolator.
Note that this flag is only relevant for the Mesos Containerizer. (default:
windows/cpu,windows/mem on Windows; posix/cpu,posix/mem on other platforms)
  </td>
</tr>

<tr id="launcher">
  <td>
    --launcher=VALUE
  </td>
  <td>
The launcher to be used for Mesos containerizer. It could either be
<code>linux</code> or <code>posix</code>. The Linux launcher is required for cgroups
isolation and for any isolators that require Linux namespaces such as
network, pid, etc. If unspecified, the agent will choose the Linux
launcher if it's running as root on Linux.
  </td>
</tr>

<tr id="launcher_dir">
  <td>
    --launcher_dir=VALUE
  </td>
  <td>
Directory path of Mesos binaries. Mesos looks for the
fetcher, containerizer, and executor binary files under this
directory. (default: /usr/local/libexec/mesos)
  </td>
</tr>

<tr id="master_detector">
  <td>
  --master_detector=VALUE
  </td>
  <td>
The symbol name of the master detector to use. This symbol should exist in a
module specified through the <code>--modules</code> flag. Cannot be used in
conjunction with <code>--master</code>.
  </td>
</tr>

<tr id="nvidia_gpu_devices">
  <td>
    --nvidia_gpu_devices=VALUE
  </td>
  <td>
A comma-separated list of Nvidia GPU devices. When <code>gpus</code> is specified
in the <code>--resources</code> flag, this flag determines which GPU devices will
be made available. The devices should be listed as numbers that
correspond to Nvidia's NVML device enumeration (as seen by running the
command <code>nvidia-smi</code> on an Nvidia GPU equipped system). The GPUs
listed will only be isolated if the <code>--isolation</code> flag contains the
string <code>gpu/nvidia</code>.
  </td>
</tr>

<tr id="network_cni_plugins_dir">
  <td>
    --network_cni_plugins_dir=VALUE
  </td>
  <td>
Directory path of the CNI plugin binaries. The <code>network/cni</code>
isolator will find CNI plugins under this directory so that it can execute
the plugins to add/delete container from the CNI networks. It is the operator's
responsibility to install the CNI plugin binaries in the specified directory.
  </td>
</tr>

<tr id="network_cni_config_dir">
  <td>
    --network_cni_config_dir=VALUE
  </td>
  <td>
Directory path of the CNI network configuration files. For each network that
containers launched in Mesos agent can connect to, the operator should install
a network configuration file in JSON format in the specified directory.
  </td>
</tr>

<tr id="network_cni_root_dir_persist">
  <td>
    --[no-]network_cni_root_dir_persist
  </td>
  <td>
This setting controls whether the CNI root directory persists across
reboot or not.
  </td>
</tr>

<tr id="network_cni_metrics">
  <td>
    --[no-]network_cni_metrics
  </td>
  <td>
This setting controls whether the networking metrics of the CNI isolator should
be exposed.
  </td>
</tr>

<tr id="oversubscribed_resources_interval">
  <td>
    --oversubscribed_resources_interval=VALUE
  </td>
  <td>
The agent periodically updates the master with the current estimation
about the total amount of oversubscribed resources that are allocated
and available. The interval between updates is controlled by this flag.
(default: 15secs)
  </td>
</tr>

<tr id="perf_duration">
  <td>
    --perf_duration=VALUE
  </td>
  <td>
Duration of a perf stat sample. The duration must be less
than the <code>perf_interval</code>. (default: 10secs)
  </td>
</tr>

<tr id="perf_events">
  <td>
    --perf_events=VALUE
  </td>
  <td>
List of command-separated perf events to sample for each container
when using the perf_event isolator. Default is none.
Run command <code>perf list</code> to see all events. Event names are
sanitized by downcasing and replacing hyphens with underscores
when reported in the PerfStatistics protobuf, e.g., <code>cpu-cycles</code>
becomes <code>cpu_cycles</code>; see the PerfStatistics protobuf for all names.
  </td>
</tr>

<tr id="perf_interval">
  <td>
    --perf_interval=VALUE
  </td>
  <td>
Interval between the start of perf stat samples. Perf samples are
obtained periodically according to <code>perf_interval</code> and the most
recently obtained sample is returned rather than sampling on
demand. For this reason, <code>perf_interval</code> is independent of the
resource monitoring interval. (default: 60secs)
  </td>
</tr>

<tr id="qos_controller">
  <td>
    --qos_controller=VALUE
  </td>
  <td>
The name of the QoS Controller to use for oversubscription.
  </td>
</tr>

<tr id="qos_correction_interval_min">
  <td>
    --qos_correction_interval_min=VALUE
  </td>
  <td>
The agent polls and carries out QoS corrections from the QoS
Controller based on its observed performance of running tasks.
The smallest interval between these corrections is controlled by
this flag. (default: 0secs)
  </td>
</tr>

<tr id="reconfiguration_policy">
  <td>
    --reconfiguration_policy=VALUE
  </td>
  <td>
This flag controls which agent configuration changes are considered
acceptable when recovering the previous agent state. Possible values:
    equal:    The old and the new state must match exactly.
    additive: The new state must be a superset of the old state:
              it is permitted to add additional resources, attributes
              and domains but not to remove or to modify existing ones.

Note that this only affects the checking done on the agent itself,
the master may still reject the agent if it detects a change that it
considers unacceptable, which, e.g., currently happens when port or hostname
are changed. (default: equal)
  </td>
</tr>

<tr id="recover">
  <td>
    --recover=VALUE
  </td>
  <td>
Whether to recover status updates and reconnect with old executors.
Valid values for <code>recover</code> are
reconnect: Reconnect with any old live executors.
cleanup  : Kill any old live executors and exit.
           Use this option when doing an incompatible agent
           or executor upgrade!). (default: reconnect)
  </td>
</tr>

<tr id="recovery_timeout">
  <td>
    --recovery_timeout=VALUE
  </td>
  <td>
Amount of time allotted for the agent to recover. If the agent takes
longer than recovery_timeout to recover, any executors that are
waiting to reconnect to the agent will self-terminate.
(default: 15mins)
  </td>
</tr>

<tr id="registration_backoff_factor">
  <td>
    --registration_backoff_factor=VALUE
  </td>
  <td>
Agent initially picks a random amount of time between <code>[0, b]</code>, where
<code>b = registration_backoff_factor</code>, to (re-)register with a new master.
Subsequent retries are exponentially backed off based on this
interval (e.g., 1st retry uses a random value between <code>[0, b * 2^1]</code>,
2nd retry between <code>[0, b * 2^2]</code>, 3rd retry between <code>[0, b * 2^3]</code>,
etc) up to a maximum of 1mins (default: 1secs)
  </td>
</tr>

<tr id="resource_estimator">
  <td>
    --resource_estimator=VALUE
  </td>
  <td>
The name of the resource estimator to use for oversubscription.
  </td>
</tr>

<tr id="resources">
  <td>
    --resources=VALUE
  </td>
  <td>
Total consumable resources per agent. Can be provided in JSON format
or as a semicolon-delimited list of key:value pairs, with the role
optionally specified.
<p/>
As a key:value list:
<code>name(role):value;name:value...</code>
<p/>
To use JSON, pass a JSON-formatted string or use
<code>--resources=filepath</code> to specify the resources via a file containing
a JSON-formatted string. 'filepath' can only be of the form
<code>file:///path/to/file</code>.
<p/>
Example JSON:
<pre><code>[
  {
    "name": "cpus",
    "type": "SCALAR",
    "scalar": {
      "value": 24
    }
  },
  {
    "name": "mem",
    "type": "SCALAR",
    "scalar": {
      "value": 24576
    }
  }
]</code></pre>
  </td>
</tr>

<tr id="resource_provider_config_dir">
  <td>
    --resource_provider_config_dir=VALUE
  </td>
  <td>
Path to a directory that contains local resource provider configs.
Each file in the config dir should contain a JSON object representing
a <code>ResourceProviderInfo</code> object. Each local resource
provider provides resources that are local to the agent. It is also
responsible for handling operations on the resources it provides.
Please note that <code>resources</code> field might not need to be
specified if the resource provider determines the resources
automatically.
<p/>
Example config file in this directory:
<pre><code>{
  "type": "org.mesos.apache.rp.local.storage",
  "name": "lvm"
}</code></pre>
  </td>
</tr>

<tr id="csi_plugin_config_dir">
  <td>
    --csi_plugin_config_dir=VALUE
  </td>
  <td>
Path to a directory that contains CSI plugin configs.
Each file in the config dir should contain a JSON object representing
a <code>CSIPluginInfo</code> object which can be either a managed CSI
plugin (i.e. the plugin launched by Mesos as a standalone container)
or an unmanaged CSI plugin (i.e. the plugin launched out of Mesos).
<p/>
Example config files in this directory:
<pre><code>{
  "type": "org.apache.mesos.csi.managed-plugin",
  "containers": [
    {
      "services": [
        "NODE_SERVICE"
      ],
      "command": {
        "value": "<path-to-managed-plugin> --endpoint=$CSI_ENDPOINT"
      },
      "resources": [
        {"name": "cpus", "type": "SCALAR", "scalar": {"value": 0.1}},
        {"name": "mem", "type": "SCALAR", "scalar": {"value": 1024}}
      ]
    }
  ]
}</code></pre>
<pre><code>{
  "type": "org.apache.mesos.csi.unmanaged-plugin",
  "endpoints": [
    {
      "csi_service": "NODE_SERVICE",
      "endpoint": "/var/lib/unmanaged-plugin/csi.sock"
    }
  ],
  "target_path_root": "/mnt/unmanaged-plugin"
}</code></pre>
  </td>
</tr>

<tr id="revocable_cpu_low_priority">
  <td>
    --[no-]revocable_cpu_low_priority
  </td>
  <td>
Run containers with revocable CPU at a lower priority than
normal containers (non-revocable cpu). Currently only
supported by the cgroups/cpu isolator. (default: true)
  </td>
</tr>

<tr id="runtime_dir">
  <td>
    --runtime_dir
  </td>
  <td>
Path of the agent runtime directory. This is where runtime data
is stored by an agent that it needs to persist across crashes (but
not across reboots). This directory will be cleared on reboot.
(Example: <code>/var/run/mesos</code>)
  </td>
</tr>

<tr id="sandbox_directory">
  <td>
    --sandbox_directory=VALUE
  </td>
  <td>
The absolute path for the directory in the container where the
sandbox is mapped to.
(default: /mnt/mesos/sandbox)
  </td>
</tr>

<tr id="strict">
  <td>
    --[no-]strict
  </td>
  <td>
If <code>strict=true</code>, any and all recovery errors are considered fatal.
If <code>strict=false</code>, any expected errors (e.g., agent cannot recover
information about an executor, because the agent died right before
the executor registered.) during recovery are ignored and as much
state as possible is recovered.
(default: true)
  </td>
</tr>

<tr id="secret_resolver">
  <td>
    --secret_resolver=VALUE
  </td>
  <td>
The name of the secret resolver module to use for resolving
environment and file-based secrets. If this flag is not specified,
the default behavior is to resolve value-based secrets and error on
reference-based secrets.
  </td>
</tr>

<tr id="switch_user">
  <td>
    --[no-]switch_user
  </td>
  <td>
If set to <code>true</code>, the agent will attempt to run tasks as
the <code>user</code> who submitted them (as defined in <code>FrameworkInfo</code>)
(this requires <code>setuid</code> permission and that the given <code>user</code>
exists on the agent).
If the user does not exist, an error occurs and the task will fail.
If set to <code>false</code>, tasks will be run as the same user as the Mesos
agent process.
<b>NOTE</b>: This feature is not yet supported on Windows agent, and
therefore the flag currently does not exist on that platform. (default: true)
  </td>
</tr>
<tr>
  <td>
    --[no-]systemd_enable_support
  </td>
  <td>
Top level control of systemd support. When enabled, features such as
executor life-time extension are enabled unless there is an explicit
flag to disable these (see other flags). This should be enabled when
the agent is launched as a systemd unit.
(default: true)
  </td>
</tr>
<tr>
  <td>
    --systemd_runtime_directory=VALUE
  </td>
  <td>
The path to the systemd system run time directory.
(default: /run/systemd/system)
  </td>
</tr>
<tr>
  <td>
    --volume_gid_range=VALUE
  </td>
  <td>
When this flag is specified, if a task running as non-root user uses a
shared persistent volume or a PARENT type SANDBOX_PATH volume, the
volume will be owned by a gid allocated from this range and have the
`setgid` bit set, and the task process will be launched with the gid
as its supplementary group to make sure it can access the volume.
(Example: <code>[10000-20000]</code>)
  </td>
</tr>
</table>

## Network Isolator Flags

*Available when configured with `--with-network-isolator`.*

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

<tr id="ephemeral_ports_per_container">
  <td>
    --ephemeral_ports_per_container=VALUE
  </td>
  <td>
Number of ephemeral ports allocated to a container by the network
isolator. This number has to be a power of 2. This flag is used
for the <code>network/port_mapping</code> isolator. (default: 1024)
  </td>
</tr>

<tr id="eth0_name">
  <td>
    --eth0_name=VALUE
  </td>
  <td>
The name of the public network interface (e.g., <code>eth0</code>). If it is
not specified, the network isolator will try to guess it based
on the host default gateway. This flag is used for the
<code>network/port_mapping</code> isolator.
  </td>
</tr>

<tr id="lo_name">
  <td>
    --lo_name=VALUE
  </td>
  <td>
The name of the loopback network interface (e.g., lo). If it is
not specified, the network isolator will try to guess it. This
flag is used for the <code>network/port_mapping</code> isolator.
  </td>
</tr>

<tr id="egress_rate_limit_per_container">
  <td>
    --egress_rate_limit_per_container=VALUE
  </td>
  <td>
The limit of the egress traffic for each container, in Bytes/s.
If not specified or specified as zero, the network isolator will
impose no limits to containers' egress traffic throughput.
This flag uses the Bytes type (defined in stout) and is used for
the <code>network/port_mapping</code> isolator.
  </td>
</tr>

<tr id="egress_unique_flow_per_container">
  <td>
    --[no-]egress_unique_flow_per_container
  </td>
  <td>
Whether to assign an individual flow for each container for the
egress traffic. This flag is used for the <code>network/port_mapping</code>
isolator. (default: false)
  </td>
</tr>

<tr id="egress_flow_classifier_parent">
  <td>
    --egress_flow_classifier_parent=VALUE
  </td>
  <td>
When <code>egress_unique_flow_per_container</code> is enabled, we need to install
a flow classifier (fq_codel) qdisc on egress side. This flag specifies
where to install it in the hierarchy. By default, we install it at root.
  </td>
</tr>

<tr id="network_enable_socket_statistics_summary">
  <td>
    --[no-]network_enable_socket_statistics_summary
  </td>
  <td>
Whether to collect socket statistics summary for each container.
This flag is used for the <code>network/port_mapping</code> isolator.
(default: false)
  </td>
</tr>

<tr id="network_enable_socket_statistics_details">
  <td>
    --[no-]network_enable_socket_statistics_details
  </td>
  <td>
Whether to collect socket statistics details (e.g., TCP RTT) for
each container. This flag is used for the <code>network/port_mapping</code>
isolator. (default: false)
  </td>
</tr>

<tr id="network_enable_snmp_statistics">
  <td>
    --[no-]network_enable_snmp_statistics
  </td>
  <td>
Whether to collect SNMP statistics details (e.g., TCPRetransSegs) for
each container. This flag is used for the 'network/port_mapping'
isolator. (default: false)
  </td>
</tr>

</table>

## Seccomp Isolator flags

*Available when configured with `--enable-seccomp-isolator`.*

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

<tr id="seccomp_config_dir">
  <td>
    --seccomp_config_dir=VALUE
  </td>
<td>
Directory path of the Seccomp profiles.
If a container is launched with a specified Seccomp profile name,
the <code>linux/seccomp</code> isolator will try to locate a Seccomp
profile in the specified directory.
</td>
</tr>

<tr id="seccomp_profile_name">
  <td>
    --seccomp_profile_name=VALUE
  </td>
<td>
Path of the default Seccomp profile relative to the <code>seccomp_config_dir</code>.
If this flag is specified, the <code>linux/seccomp</code> isolator applies the Seccomp
profile by default when launching a new Mesos container.
<b>NOTE</b>: A Seccomp profile must be compatible with the
Docker Seccomp profile format (e.g., https://github.com/moby/moby/blob/master/profiles/seccomp/default.json).
</td>
</tr>

</table>

## XFS Disk Isolator flags

*Available when configured with `--enable-xfs-disk-isolator`.*

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

<tr id="xfs_project_range">
  <td>
    --xfs_project_range=VALUE
  </td>
<td>
The ranges of XFS project IDs that the isolator can use to track disk
quotas for container sandbox directories. Valid project IDs range from
1 to max(uint32). (default `[5000-10000]`)
</td>
</tr>

</table>
