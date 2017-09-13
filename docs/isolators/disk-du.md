---
title: Apache Mesos - Disk Isolator Based on 'du' in Mesos Containerizer
layout: documentation
---

# Disk Isolator Based on 'du' in Mesos Containerizer

The `disk/du` (previously `posix/disk`) isolator provides basic disk
isolation. It is able to report the disk usage for each sandbox and
optionally enforce the disk quota. It can be used on both Linux and OS
X.

To enable the `disk/du` isolator, append `disk/du` to the
`--isolation` flag when starting the agent.

By default, the disk quota enforcement is disabled. To enable it,
specify `--enforce_container_disk_quota` when starting the agent.

The `disk/du` isolator reports disk usage for each sandbox by
periodically running the `du` command. The disk usage can be retrieved
from the resource statistics endpoint
([/monitor/statistics](../endpoints/slave/monitor/statistics.md)).

The interval between two `du`s can be controlled by the agent flag
`--container_disk_watch_interval`. For example,
`--container_disk_watch_interval=1mins` sets the interval to be 1
minute. The default interval is 15 seconds.
