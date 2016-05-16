---
title: Apache Mesos - HTTP Endpoints - /containers
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /containers
>        /slave(1)/containers

### TL;DR; ###
Retrieve container status and usage information.

### DESCRIPTION ###
Returns the current resource consumption data and status for
containers running under this slave.

Example (**Note**: this is not exhaustive):

```
[{
    "container_id":"container",
    "container_status":
    {
        "network_infos":
        [{"ip_addresses":[{"ip_address":"192.168.1.1"}]}]
    }
    "executor_id":"executor",
    "executor_name":"name",
    "framework_id":"framework",
    "source":"source",
    "statistics":
    {
        "cpus_limit":8.25,
        "cpus_nr_periods":769021,
        "cpus_nr_throttled":1046,
        "cpus_system_time_secs":34501.45,
        "cpus_throttled_time_secs":352.597023453,
        "cpus_user_time_secs":96348.84,
        "mem_anon_bytes":4845449216,
        "mem_file_bytes":260165632,
        "mem_limit_bytes":7650410496,
        "mem_mapped_file_bytes":7159808,
        "mem_rss_bytes":5105614848,
        "timestamp":1388534400.0
    }
}]
```


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.