---
title: Apache Mesos - HTTP Endpoints - /state
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /state
>        /master/state

### TL;DR; ###
Information about state of master.

### DESCRIPTION ###
Returns 200 OK when the state of the master was queried successfully.

Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.

Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.

This endpoint shows information about the frameworks, tasks,
executors, and agents running in the cluster as a JSON object.
The information shown might be filtered based on the user
accessing the endpoint.

Example (**Note**: this is not exhaustive):

```
{
    "version" : "0.28.0",
    "git_sha" : "9d5889b5a265849886a533965f4aefefd1fbd103",
    "git_branch" : "refs/heads/master",
    "git_tag" : "0.28.0",
    "build_date" : "2016-02-15 10:00:28",
    "build_time" : 1455559228,
    "build_user" : "mesos-user",
    "start_time" : 1455643643.42422,
    "elected_time" : 1455643643.43457,
    "id" : "b5eac2c5-609b-4ca1-a352-61941702fc9e",
    "pid" : "master@127.0.0.1:5050",
    "hostname" : "localhost",
    "activated_slaves" : 0,
    "deactivated_slaves" : 0,
    "cluster" : "test-cluster",
    "leader" : "master@127.0.0.1:5050",
    "log_dir" : "/var/log",
    "external_log_file" : "mesos.log",
    "flags" : {
         "framework_sorter" : "drf",
         "authenticate" : "false",
         "logbufsecs" : "0",
         "initialize_driver_logging" : "true",
         "work_dir" : "/var/lib/mesos",
         "http_authenticators" : "basic",
         "authorizers" : "local",
         "agent_reregister_timeout" : "10mins",
         "logging_level" : "INFO",
         "help" : "false",
         "root_submissions" : "true",
         "ip" : "127.0.0.1",
         "user_sorter" : "drf",
         "version" : "false",
         "max_agent_ping_timeouts" : "5",
         "agent_ping_timeout" : "15secs",
         "registry_store_timeout" : "20secs",
         "max_completed_frameworks" : "50",
         "quiet" : "false",
         "allocator" : "HierarchicalDRF",
         "hostname_lookup" : "true",
         "authenticators" : "crammd5",
         "max_completed_tasks_per_framework" : "1000",
         "registry" : "replicated_log",
         "registry_strict" : "false",
         "log_auto_initialize" : "true",
         "authenticate_agents" : "false",
         "registry_fetch_timeout" : "1mins",
         "allocation_interval" : "1secs",
         "authenticate_http" : "false",
         "port" : "5050",
         "zk_session_timeout" : "10secs",
         "recovery_agent_removal_limit" : "100%",
         "webui_dir" : "/path/to/mesos/build/../src/webui",
         "cluster" : "mycluster",
         "leader" : "master@127.0.0.1:5050",
         "log_dir" : "/var/log",
         "external_log_file" : "mesos.log"
    },
    "slaves" : [],
    "frameworks" : [],
    "completed_frameworks" : [],
    "orphan_tasks" : [],
    "unregistered_frameworks" : []
}
```


### AUTHENTICATION ###
This endpoint requires authentication if HTTP authentication is
enabled.

### AUTHORIZATION ###
This endpoint might be filtered based on the user accessing it.
For example a user might only see the subset of frameworks,
tasks, and executors they are allowed to view.
See the authorization documentation for details.