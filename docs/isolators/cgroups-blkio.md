---
title: Apache Mesos - Cgroups 'blkio' Subsystem Support in Mesos Containerizer
layout: documentation
---

# Cgroups 'blkio' Subsystem Support in Mesos Containerizer

The `cgroups/blkio` isolator provides block I/O performance isolation for
containers through [the *blkio* Linux cgroup subsystem](https://www.kernel.org/doc/Documentation/cgroup-v1/blkio-controller.txt).
To enable the isolator, append `cgroups/blkio` to the `--isolation` flag before
starting the agent.

The blkio subsystem enables I/O statistics collection and allows operators to
apply I/O control policies for block devices. The isolator places the processes
of a Mesos container into a separate blkio cgroup hierarchy. At the moment,
it only supports reporting containers' I/O statistics on block devices to
the agent. A sample statistics would be something like:

```
[{
    "executor_id": "executor",
    "executor_name": "name",
    "framework_id": "framework",
    "source": "source",
    "statistics": {
        "blkio": {
            "cfq": [
                {
                    "io_merged": [
                        {
                            "op": "TOTAL",
                            "value": 0
                        }
                    ],
                    "io_queued": [
                        {
                            "op": "TOTAL",
                            "value": 0
                        }
                    ],
                    "io_service_bytes": [
                        {
                            "op": "TOTAL",
                            "value": 0
                        }
                    ],
                    "io_service_time": [
                        {
                            "op": "TOTAL",
                            "value": 0
                        }
                    ],
                    "io_serviced": [
                        {
                            "op": "TOTAL",
                            "value": 0
                        }
                    ],
                    "io_wait_time": [
                        {
                            "op": "TOTAL",
                            "value": 0
                        }
                    ]
                }
            ],
            "cfq_recursive": [
                {
                    "io_merged": [
                        {
                            "op": "TOTAL",
                            "value": 0
                        }
                    ],
                    "io_queued": [
                        {
                            "op": "TOTAL",
                            "value": 0
                        }
                    ],
                    "io_service_bytes": [
                        {
                            "op": "TOTAL",
                            "value": 0
                        }
                    ],
                    "io_service_time": [
                        {
                            "op": "TOTAL",
                            "value": 0
                        }
                    ],
                    "io_serviced": [
                        {
                            "op": "TOTAL",
                            "value": 0
                        }
                    ],
                    "io_wait_time": [
                        {
                            "op": "TOTAL",
                            "value": 0
                        }
                    ]
                }
            ],
            "throttling": [
                {
                    "device": {
                        "major": 8,
                        "minor": 0
                    },
                    "io_service_bytes": [
                        {
                            "op": "READ",
                            "value": 0
                        },
                        {
                            "op": "WRITE",
                            "value": 4096
                        },
                        {
                            "op": "SYNC",
                            "value": 0
                        },
                        {
                            "op": "ASYNC",
                            "value": 4096
                        },
                        {
                            "op": "TOTAL",
                            "value": 4096
                        }
                    ],
                    "io_serviced": [
                        {
                            "op": "READ",
                            "value": 0
                        },
                        {
                            "op": "WRITE",
                            "value": 1
                        },
                        {
                            "op": "SYNC",
                            "value": 0
                        },
                        {
                            "op": "ASYNC",
                            "value": 1
                        },
                        {
                            "op": "TOTAL",
                            "value": 1
                        }
                    ]
                },
                {
                    "io_service_bytes": [
                        {
                            "op": "TOTAL",
                            "value": 4096
                        }
                    ],
                    "io_serviced": [
                        {
                            "op": "TOTAL",
                            "value": 1
                        }
                    ]
                }
            ]
        },
        "cpus_limit": 1.1,
        "mem_limit_bytes": 167772160,
        "timestamp": 1500335339.30187
    }
}]
```

For more details about the blkio subsystem, please refer to
the [Block I/O Controller](https://www.kernel.org/doc/Documentation/cgroup-v1/blkio-controller.txt)
Linux kernel documentation.
