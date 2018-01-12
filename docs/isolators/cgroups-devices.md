---
title: Apache Mesos - Cgroups 'devices' Subsystem Support in Mesos Containerizer
layout: documentation
---

# Cgroups 'devices' Subsystems Support in Mesos Containerizer

The `cgroups/devices` isolator allows operators to provide device isolation for
containers launched by Mesos Containerizer. It uses the cgroups
[device whitelist controller](https://www.kernel.org/doc/Documentation/cgroup-v1/devices.txt) to
track and enforce open and mknod restrictions on device files. To enable the
`cgroups/devices` isolator, append `cgroups/devices` to the `--isolation` flag
when starting the Mesos agent.

## Default whitelisted devices

The following devices are, by default, whitelisted for each container, if you
turn on this isolator.

Each whitelist entry has 4 fields. `type` is `a` (all), `c` (char), or `b`
(block). 'all' means it applies to all types and all major and minor numbers.
Major and minor are either an integer or `*` for all.  Access is a composition
of `r` ([read](http://man7.org/linux/man-pages/man2/read.2.html)),
`w` ([write](http://man7.org/linux/man-pages/man2/write.2.html)),
and `m` ([mknod](http://man7.org/linux/man-pages/man2/mknod.2.html)).

* `c *:* m`: Make new character devices using [mknod(2)](http://man7.org/linux/man-pages/man2/mknod.2.html).
* `b *:* m`: Make new block devices using [mknod(2)](http://man7.org/linux/man-pages/man2/mknod.2.html).
* `c 5:1 rwm`: Read/write `/dev/console`
* `c 4:0 rwm`: Read/write `/dev/tty0`
* `c 4:1 rwm`: Read/write `/dev/tty1`
* `c 136:* rwm`: Read/write `/dev/pts/*`
* `c 5:2 rwm`: Read/write `/dev/ptmx`
* `c 10:200 rwm`: Read/write `/dev/net/tun`
* `c 1:3 rwm`: Read/write `/dev/null`
* `c 1:5 rwm`: Read/write `/dev/zero`
* `c 1:7 rwm`: Read/write `/dev/full`
* `c 5:0 rwm`: Read/write `/dev/tty`
* `c 1:9 rwm`: Read/write `/dev/urandom`
* `c 1:8 rwm`: Read/write `/dev/random`

Note that the cgroups device whitelist control is based on device numbers. This
is orthogonal to populating `/dev`, which is typically done by udev or devtmpfs.

Capability `CAP_MKNOD` is always required to perform
[mknod(2)](http://man7.org/linux/man-pages/man2/mknod.2.html) irrespective of
whether the device is whitelisted or not.

## Additional whitelisted devices

The operator can configure the agent to add additional whitelisted devices using
the `--allowed_devices` flag on the agent. The flag takes a JSON object (or the
path to a file that contains the JSON object). For example:

```json
{
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
```
