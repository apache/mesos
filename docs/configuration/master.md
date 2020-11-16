---
title: Apache Mesos - Master Options
layout: documentation
---

# Master Options

## Required Flags

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

<tr id="quorum">
  <td>
    --quorum=VALUE
  </td>
  <td>
The size of the quorum of replicas when using <code>replicated_log</code> based
registry. It is imperative to set this value to be a majority of
masters i.e., <code>quorum > (number of masters)/2</code>.
<b>NOTE</b>: Not required if master is run in standalone mode (non-HA).
  </td>
</tr>

<tr id="work_dir">
  <td>
    --work_dir=VALUE
  </td>
  <td>
Path of the master work directory. This is where the persistent
information of the cluster will be stored. Note that locations like
<code>/tmp</code> which are cleaned automatically are not suitable for the work
directory when running in production, since long-running masters could
lose data when cleanup occurs. (Example: <code>/var/lib/mesos/master</code>)
  </td>
</tr>

<tr id="zk">
  <td>
    --zk=VALUE
  </td>
  <td>
ZooKeeper URL (used for leader election amongst masters).
May be one of:
<pre><code>zk://host1:port1,host2:port2,.../path
zk://username:password@host1:port1,host2:port2,.../path
file:///path/to/file (where file contains one of the above)</code></pre>
<b>NOTE</b>: Not required if master is run in standalone mode (non-HA).
  </td>
</tr>

</table>

## Optional Flags

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
Note that if the flag <code>--authorizers</code> is provided with a value
different than <code>local</code>, the ACLs contents will be
ignored.
<p/>
See the ACLs protobuf in acls.proto for the expected format.
<p/>
Example:
<pre><code>{
  "register_frameworks": [
    {
      "principals": { "type": "ANY" },
      "roles": { "values": ["a"] }
    }
  ],
  "run_tasks": [
    {
      "principals": { "values": ["a", "b"] },
      "users": { "values": ["c"] }
    }
  ],
  "teardown_frameworks": [
    {
      "principals": { "values": ["a", "b"] },
      "framework_principals": { "values": ["c"] }
    }
  ],
  "set_quotas": [
    {
      "principals": { "values": ["a"] },
      "roles": { "values": ["a", "b"] }
    }
  ],
  "remove_quotas": [
    {
      "principals": { "values": ["a"] },
      "quota_principals": { "values": ["a"] }
    }
  ],
  "get_endpoints": [
    {
      "principals": { "values": ["a"] },
      "paths": { "values": ["/flags"] }
    }
  ]
}</code></pre>
  </td>
</tr>

<tr id="agent_ping_timeout">
  <td>
    --agent_ping_timeout=VALUE,
    <p/>
    --slave_ping_timeout=VALUE
  </td>
  <td>
The timeout within which an agent is expected to respond to a
ping from the master. Agents that do not respond within
max_agent_ping_timeouts ping retries will be marked unreachable.
<b>NOTE</b>: The total ping timeout (<code>agent_ping_timeout</code> multiplied by
<code>max_agent_ping_timeouts</code>) should be greater than the ZooKeeper
session timeout to prevent useless re-registration attempts.
(default: 15secs)
  </td>
</tr>

<tr id="agent_removal_rate_limit">
  <td>
    --agent_removal_rate_limit=VALUE
    <p/>
    --slave_removal_rate_limit=VALUE
  </td>
  <td>
The maximum rate (e.g., <code>1/10mins</code>, <code>2/3hrs</code>, etc) at which agents
will be removed from the master when they fail health checks.
By default, agents will be removed as soon as they fail the health
checks. The value is of the form <code>(Number of agents)/(Duration)</code>.
  </td>
</tr>

<tr id="agent_reregister_timeout">
  <td>
    --agent_reregister_timeout=VALUE
    <p/>
    --slave_reregister_timeout=VALUE
  </td>
  <td>
The timeout within which an agent is expected to reregister.
Agents reregister when they become disconnected from the master
or when a new master is elected as the leader. Agents that do not
reregister within the timeout will be marked unreachable in the
registry; if/when the agent reregisters with the master, any
non-partition-aware tasks running on the agent will be terminated.
<b>NOTE</b>: This value has to be at least 10mins. (default: 10mins)
  </td>
</tr>

<tr id="allocation_interval">
  <td>
    --allocation_interval=VALUE
  </td>
  <td>
Amount of time to wait between performing
(batch) allocations (e.g., 500ms, 1sec, etc). (default: 1secs)
  </td>
</tr>

<tr id="allocator">
  <td>
    --allocator=VALUE
  </td>
  <td>
Allocator to use for resource allocation to frameworks.
Use the default <code>HierarchicalDRF</code> allocator, or
load an alternate allocator module using <code>--modules</code>.
(default: HierarchicalDRF)
  </td>
</tr>

<tr id="min_allocatable_resources">
  <td>
    --min_allocatable_resources=VALUE
  </td>
  <td>
One or more sets of resource quantities that define the minimum allocatable
resource for the allocator. The allocator will only offer resources that
meets the quantity requirement of at least one of the specified sets. For
`SCALAR` type resources, its quantity is its scalar value. For `RANGES` and
`SET` type, their quantities are the number of different instances in the
range or set. For example, `range:[1-5]` has a quantity of 5 and `set:{a,b}`
has a quantity of 2. The resources in each set should be delimited by
semicolons (acting as logical AND), and each set should be delimited by the
pipe character (acting as logical OR).
(Example: `disk:1|cpus:1;mem:32;ports:1` configures the allocator to only offer
resources if they contain a disk resource of at least 1 megabyte, or if they
at least contain 1 cpu, 32 megabytes of memory and 1 port.)
(default: cpus:0.01|mem:32).
  </td>
</tr>

<tr id="authenticate_agents">
  <td>
    --[no-]authenticate_agents,
    <p/>
    --[no-]authenticate_slaves
  </td>
  <td>
If <code>true</code> only authenticated agents are allowed to register.
If <code>false</code> unauthenticated agents are also allowed to register. (default: false)
  </td>
</tr>

<tr id="authenticate_frameworks">
  <td>
    --[no-]authenticate_frameworks,
    <p/>
    --[no-]authenticate
  </td>
  <td>
If <code>true</code>, only authenticated frameworks are allowed to register. If
<code>false</code>, unauthenticated frameworks are also allowed to register. For
HTTP based frameworks use the <code>--authenticate_http_frameworks</code> flag. (default: false)
  </td>
</tr>

<tr id="authenticate_http_frameworks">
  <td>
    --[no-]authenticate_http_frameworks
  </td>
  <td>
If <code>true</code>, only authenticated HTTP based frameworks are allowed to
register. If <code>false</code>, HTTP frameworks are not authenticated. (default: false)
  </td>
</tr>

<tr id="authenticators">
  <td>
    --authenticators=VALUE
  </td>
  <td>
Authenticator implementation to use when authenticating frameworks
and/or agents. Use the default <code>crammd5</code>, or
load an alternate authenticator module using <code>--modules</code>. (default: crammd5)
  </td>
</tr>

<tr id="authentication_v0_timeout">
  <td>
    --authentication_v0_timeout=VALUE
  </td>
  <td>
The timeout within which an authentication is expected to complete against a v0 framework or agent. This does not apply to the v0 or v1 HTTP APIs. (default: <code>15secs</code>)
  </td>
</tr>

<tr id="authorizers">
  <td>
    --authorizers=VALUE
  </td>
  <td>
Authorizer implementation to use when authorizing actions that
require it.
Use the default <code>local</code>, or
load an alternate authorizer module using <code>--modules</code>.
<p/>
Note that if the flag <code>--authorizers</code> is provided with a value
different than the default <code>local</code>, the ACLs
passed through the <code>--acls</code> flag will be ignored.
<p/>
Currently there is no support for multiple authorizers. (default: local)
  </td>
</tr>

<tr id="cluster">
  <td>
    --cluster=VALUE
  </td>
  <td>
Human readable name for the cluster, displayed in the webui.
  </td>
</tr>

<tr id="credentials">
  <td>
    --credentials=VALUE
  </td>
  <td>
Path to a JSON-formatted file containing credentials.
Path can be of the form <code>file:///path/to/file</code> or <code>/path/to/file</code>.
Example:
<pre><code>{
  "credentials": [
    {
      "principal": "sherman",
      "secret": "kitesurf"
    }
  ]
}</code></pre>
  </td>
</tr>

<tr id="fair_sharing_excluded_resource_names">
  <td>
    --fair_sharing_excluded_resource_names=VALUE
  </td>
  <td>
A comma-separated list of the resource names (e.g. 'gpus') that will be excluded
from fair sharing constraints. This may be useful in cases where the fair
sharing implementation currently has limitations. E.g. See the problem of
"scarce" resources:
    <a href="http://www.mail-archive.com/dev@mesos.apache.org/msg35631.html">msg35631</a>
    <a href="https://issues.apache.org/jira/browse/MESOS-5377">MESOS-5377</a>
  </td>
</tr>

<tr id="filter_gpu_resources">
  <td>
    --[no-]filter_gpu_resources
  </td>
  <td>
When set to true, this flag will cause the mesos master to filter all offers
from agents with GPU resources by only sending them to frameworks that opt into
the 'GPU_RESOURCES' framework capability. When set to false, this flag will
cause the master to not filter offers from agents with GPU resources, and
indiscriminately send them to all frameworks whether they set the
'GPU_RESOURCES' capability or not.  This flag is meant as a temporary workaround
towards the eventual deprecation of the 'GPU_RESOURCES' capability. Please see
the following for more information:
    <a href="https://www.mail-archive.com/dev@mesos.apache.org/msg37571.html">msg37571</a>
    <a href="https://issues.apache.org/jira/browse/MESOS-7576">MESOS-7576</a>
  </td>
</tr>

<tr id="framework_sorter">
  <td>
    --framework_sorter=VALUE
  </td>
  <td>
Policy to use for allocating resources between a given role's
frameworks. Options are the same as for <code>--role_sorter</code>.
(default: drf)
  </td>
</tr>

<tr id="http_framework_authenticators">
  <td>
    --http_framework_authenticators=VALUE
  </td>
  <td>
HTTP authenticator implementation to use when authenticating HTTP frameworks.
Use the <code>basic</code> authenticator or load an alternate HTTP authenticator
module using <code>--modules</code>. This must be used in conjunction with
<code>--authenticate_http_frameworks</code>.
<p/>
Currently there is no support for multiple HTTP authenticators.
  </td>
</tr>

<tr id="log_auto_initialize">
  <td>
    --[no-]log_auto_initialize
  </td>
  <td>
Whether to automatically initialize the [replicated log](../replicated-log-internals.md)
used for the registry. If this is set to false, the log has to be manually
initialized when used for the very first time. (default: true)
  </td>
</tr>

<tr id="master_contender">
  <td>
    --master_contender=VALUE
  </td>
  <td>
The symbol name of the master contender to use. This symbol should exist in a
module specified through the <code>--modules</code> flag. Cannot be used in
conjunction with <code>--zk</code>. Must be used in conjunction with
<code>--master_detector</code>.
  </td>
</tr>

<tr id="master_detector">
  <td>
    --master_detector=VALUE
  </td>
  <td>
The symbol name of the master detector to use. This symbol should exist in a
module specified through the <code>--modules</code> flag. Cannot be used in
conjunction with <code>--zk</code>. Must be used in conjunction with
<code>--master_contender</code>.
  </td>
</tr>

<tr id="max_agent_ping_timeouts">
  <td>
    --max_agent_ping_timeouts=VALUE,
    <p/>
    --max_slave_ping_timeouts=VALUE
  </td>
  <td>
The number of times an agent can fail to respond to a
ping from the master. Agents that do not respond within
<code>max_agent_ping_timeouts</code> ping retries will be marked unreachable.
(default: 5)
  </td>
</tr>

<tr id="max_completed_frameworks">
  <td>
    --max_completed_frameworks=VALUE
  </td>
  <td>
Maximum number of completed frameworks to store in memory. (default: 50)
  </td>
</tr>

<tr id="max_completed_tasks_per_framework">
  <td>
    --max_completed_tasks_per_framework=VALUE
  </td>
  <td>
Maximum number of completed tasks per framework to store in memory. (default: 1000)
  </td>
</tr>

<tr id="max_operator_event_stream_subscribers">
  <td>
    --max_operator_event_stream_subscribers=VALUE
  </td>
  <td>
Maximum number of simultaneous subscribers to the master's operator event
stream. If new connections bring the total number of subscribers over this
value, older connections will be closed by the master.

This flag should generally not be changed unless the operator is mitigating
known problems with their network setup, such as clients/proxies that do not
close connections to the master.
(default: 1000)
  </td>
</tr>

<tr id="max_unreachable_tasks_per_framework">
  <td>
    --max_unreachable_tasks_per_framework=VALUE
  </td>
  <td>
Maximum number of unreachable tasks per framework to store in memory. (default: 1000)
  </td>
</tr>

<tr id="offer_timeout">
  <td>
    --offer_timeout=VALUE
  </td>
  <td>
Duration of time before an offer is rescinded from a framework.
This helps fairness when running frameworks that hold on to offers,
or frameworks that accidentally drop offers.
If not set, offers do not timeout.
  </td>
</tr>

<tr id="offer_constraints_re2_max_mem">
  <td>
    --offer_constraints_re2_max_mem=VALUE
  </td>
  <td>
Limit on the memory usage of each RE2 regular expression in
framework's offer constraints. If `OfferConstraints` contain a regex
from which a RE2 object cannot be constructed without exceeding this
limit, then framework's attempt to subscribe or update subscription
with these `OfferConstraints` will fail. (default: 4KB)
  </td>
</tr>

<tr id="offer_constraints_re2_max_program_size">
  <td>
    --offer_constraints_re2_max_program_size=VALUE
  </td>
  <td>
Limit on the RE2 program size of each regular expression in
framework's offer constraints. If `OfferConstraints` contain a regex
which results in a RE2 object exceeding this limit,
then framework's attempt to subscribe or update subscription
with these `OfferConstraints` will fail. (default: 100)
  </td>
</tr>

<tr id="publish_per_framework_metrics">
  <td>
    --[no-]publish_per_framework_metrics
  </td>
  <td>
If <code>true</code>, an extensive set of metrics for each active framework will
be published. These metrics are useful for understanding cluster behavior,
but can be overwhelming for very large numbers of frameworks. (default: true)
  </td>
</tr>

<tr id="rate_limits">
  <td>
    --rate_limits=VALUE
  </td>
  <td>
The value could be a JSON-formatted string of rate limits
or a file path containing the JSON-formatted rate limits used
for framework rate limiting.
Path could be of the form <code>file:///path/to/file</code>
or <code>/path/to/file</code>.
<p/>
See the RateLimits protobuf in mesos.proto for the expected format.
<p/>
Example:
<pre><code>{
  "limits": [
    {
      "principal": "foo",
      "qps": 55.5
    },
    {
      "principal": "bar"
    }
  ],
  "aggregate_default_qps": 33.3
}</code></pre>
  </td>
</tr>

<tr id="recovery_agent_removal_limit">
  <td>
    --recovery_agent_removal_limit=VALUE,
    <p/>
    --recovery_slave_removal_limit=VALUE
  </td>
  <td>
For failovers, limit on the percentage of agents that can be removed
from the registry *and* shutdown after the re-registration timeout
elapses. If the limit is exceeded, the master will fail over rather
than remove the agents.
This can be used to provide safety guarantees for production
environments. Production environments may expect that across master
failovers, at most a certain percentage of agents will fail
permanently (e.g. due to rack-level failures).
Setting this limit would ensure that a human needs to get
involved should an unexpected widespread failure of agents occur
in the cluster.
Values: [0%-100%] (default: 100%)
  </td>
</tr>

<tr id="registry">
  <td>
    --registry=VALUE
  </td>
  <td>
Persistence strategy for the registry; available options are
<code>replicated_log</code>, <code>in_memory</code> (for testing). (default: replicated_log)
  </td>
</tr>

<tr id="registry_fetch_timeout">
  <td>
    --registry_fetch_timeout=VALUE
  </td>
  <td>
Duration of time to wait in order to fetch data from the registry
after which the operation is considered a failure. (default: 1mins)
  </td>
</tr>

<tr id="registry_gc_interval">
  <td>
    --registry_gc_interval=VALUE
  </td>
  <td>
How often to garbage collect the registry. The current leading
master will periodically discard information from the registry.
How long registry state is retained is controlled by other
parameters (e.g., <code>registry_max_agent_age</code>,
<code>registry_max_agent_count</code>); this parameter controls
how often the master will examine the registry to see if data
should be discarded. (default: 15mins)
  </td>
</tr>

<tr id="registry_max_agent_age">
  <td>
    --registry_max_agent_age=VALUE
  </td>
  <td>
Maximum length of time to store information in the registry about
agents that are not currently connected to the cluster. This
information allows frameworks to determine the status of unreachable
and gone agents. Note that the registry always stores
information on all connected agents. If there are more than
<code>registry_max_agent_count</code> partitioned/gone agents, agent
information may be discarded from the registry sooner than indicated
by this parameter. (default: 2weeks)
  </td>
</tr>

<tr id="registry_max_agent_count">
  <td>
    --registry_max_agent_count=VALUE
  </td>
  <td>
Maximum number of partitioned/gone agents to store in the
registry. This information allows frameworks to determine the status
of disconnected agents. Note that the registry always stores
information about all connected agents. See also the
<code>registry_max_agent_age</code> flag. (default: 102400)
  </td>
</tr>

<tr id="registry_store_timeout">
  <td>
    --registry_store_timeout=VALUE
  </td>
  <td>
Duration of time to wait in order to store data in the registry
after which the operation is considered a failure. (default: 20secs)
  </td>
</tr>

<tr id="require_agent_domain">
  <td>
    --[no-]require_agent_domain
  </td>
  <td>
If true, only agents with a configured domain can register. (default: false)
  </td>
</tr>

<tr id="roles">
  <td>
    --roles=VALUE
  </td>
  <td>
A comma-separated list of the allocation roles that frameworks
in this cluster may belong to. This flag is deprecated;
if it is not specified, any role name can be used.
  </td>
</tr>

<tr id="root_submissions">
  <td>
    --[no-]root_submissions
  </td>
  <td>
Can root submit frameworks? (default: true)
  </td>
</tr>

<tr id="role_sorter">
  <td>
    --role_sorter=VALUE
  </td>
  <td>
Policy to use for allocating resources between roles. May be one of:
dominant_resource_fairness (drf) or weighted random uniform distribution
(random) (default: drf)
  </td>
</tr>

<tr id="webui_dir">
  <td>
    --webui_dir=VALUE
  </td>
  <td>
Directory path of the webui files/assets (default: /usr/local/share/mesos/webui)
  </td>
</tr>

<tr id="weights">
  <td>
    --weights=VALUE
  </td>
  <td>
A comma-separated list of role/weight pairs of the form
<code>role=weight,role=weight</code>. Weights can be used to control the
relative share of cluster resources that is offered to different roles. This
flag is deprecated. Instead, operators should configure weights dynamically
using the <code>/weights</code> HTTP endpoint.
  </td>
</tr>

<tr id="whitelist">
  <td>
    --whitelist=VALUE
  </td>
  <td>
Path to a file which contains a list of agents (one per line) to
advertise offers for. The file is watched and periodically re-read to
refresh the agent whitelist. By default there is no whitelist: all
machines are accepted. Path can be of the form
<code>file:///path/to/file</code> or <code>/path/to/file</code>.
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

<tr id="max_executors_per_agent">
  <td>
    --max_executors_per_agent=VALUE,
    <p/>
     --max_executors_per_slave=VALUE
  </td>
  <td>
Maximum number of executors allowed per agent. The network
monitoring/isolation technique imposes an implicit resource
acquisition on each executor (# ephemeral ports), as a result
one can only run a certain number of executors on each agent.
  </td>
</tr>

</table>
