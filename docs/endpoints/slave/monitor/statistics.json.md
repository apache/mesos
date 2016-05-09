---
title: Apache Mesos - HTTP Endpoints - /monitor/statistics.json
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /monitor/statistics.json
>        /slave(1)/monitor/statistics.json

### TL;DR; ###
Retrieve resource monitoring information.

### DESCRIPTION ###
Returns the current resource consumption data for containers
running under this agent.

Example:

```
[{
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

### AUTHORIZATION ###
The request principal should be authorized to query this endpoint.
See the authorization documentation for details.