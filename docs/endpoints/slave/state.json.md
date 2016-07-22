---
title: Apache Mesos - HTTP Endpoints - /state.json
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /state.json
>        /slave(1)/state.json

### TL;DR; ###
Information about state of the Agent.

### DESCRIPTION ###
This endpoint shows information about the frameworks, executors
and the agent's master as a JSON object.
The information shown might be filtered based on the user
accessing the endpoint.

Example (**Note**: this is not exhaustive):

```
{
    "version" : "0.28.0",
    "git_sha" : "9d5889b5a265849886a533965f4aefefd1fbd103",
    "git_branch" : "refs/heads/master",
    "git_tag" : "0.28.0",
    "build_date" : "2016-02-15 10:00:28"
    "build_time" : 1455559228,
    "build_user" : "mesos-user",
    "start_time" : 1455647422.88396,
    "id" : "e2c38084-f6ea-496f-bce3-b6e07cea5e01-S0",
    "pid" : "slave(1)@127.0.1.1:5051",
    "hostname" : "localhost",
    "resources" : {
         "ports" : "[31000-32000]",
         "mem" : 127816,
         "disk" : 804211,
         "cpus" : 32
    },
    "attributes" : {},
    "master_hostname" : "localhost",
    "log_dir" : "/var/log",
    "external_log_file" : "mesos.log",
    "frameworks" : [],
    "completed_frameworks" : [],
    "flags" : {
         "gc_disk_headroom" : "0.1",
         "isolation" : "posix/cpu,posix/mem",
         "containerizers" : "mesos",
         "docker_socket" : "/var/run/docker.sock",
         "gc_delay" : "1weeks",
         "docker_remove_delay" : "6hrs",
         "port" : "5051",
         "systemd_runtime_directory" : "/run/systemd/system",
         "initialize_driver_logging" : "true",
         "cgroups_root" : "mesos",
         "fetcher_cache_size" : "2GB",
         "cgroups_hierarchy" : "/sys/fs/cgroup",
         "qos_correction_interval_min" : "0ns",
         "cgroups_cpu_enable_pids_and_tids_count" : "false",
         "sandbox_directory" : "/mnt/mesos/sandbox",
         "docker" : "docker",
         "help" : "false",
         "docker_stop_timeout" : "0ns",
         "master" : "127.0.0.1:5050",
         "logbufsecs" : "0",
         "docker_registry" : "https://registry-1.docker.io",
         "frameworks_home" : "",
         "cgroups_enable_cfs" : "false",
         "perf_interval" : "1mins",
         "docker_kill_orphans" : "true",
         "switch_user" : "true",
         "logging_level" : "INFO",
         "hadoop_home" : "",
         "strict" : "true",
         "executor_registration_timeout" : "1mins",
         "recovery_timeout" : "15mins",
         "revocable_cpu_low_priority" : "true",
         "docker_store_dir" : "/tmp/mesos/store/docker",
         "image_provisioner_backend" : "copy",
         "authenticatee" : "crammd5",
         "quiet" : "false",
         "executor_shutdown_grace_period" : "5secs",
         "fetcher_cache_dir" : "/tmp/mesos/fetch",
         "default_role" : "*",
         "work_dir" : "/tmp/mesos",
         "launcher_dir" : "/path/to/mesos/build/src",
         "registration_backoff_factor" : "1secs",
         "oversubscribed_resources_interval" : "15secs",
         "enforce_container_disk_quota" : "false",
         "container_disk_watch_interval" : "15secs",
         "disk_watch_interval" : "1mins",
         "cgroups_limit_swap" : "false",
         "hostname_lookup" : "true",
         "perf_duration" : "10secs",
         "appc_store_dir" : "/tmp/mesos/store/appc",
         "recover" : "reconnect",
         "version" : "false"
    },
}
```


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
This endpoint might be filtered based on the user accessing it.
For example a user might only see the subset of frameworks,
tasks, and executors they are allowed to view.
See the authorization documentation for details.