---
title: Apache Mesos - Performance Profiling
layout: documentation
---

# Performance Profiling

This document over time will be home to various guides on how to use various profiling tools to do performance analysis of Mesos.

## Flamescope

[Flamescope](https://github.com/Netflix/flamescope) is a visualization tool for exploring different time ranges as [flamegraphs](https://github.com/brendangregg/FlameGraph). In order to use the tool, you first need to obtain stack traces, here's how to obtain a 60 second recording of the mesos master process at 100 hertz using Linux perf:

```
$ sudo perf record --freq=100 --no-inherit --call-graph dwarf -p <mesos-master-pid> -- sleep 60
$ sudo perf script --header | c++filt > mesos-master.stacks
$ gzip mesos-master.stacks
```

If you'd like to solicit help in analyzing the performance data, upload the `mesos-master.stacks.gz` to a publicly accessible location and file with `dev@mesos.apache.org` for analysis, or send the file over [slack](http://mesos.slack.com) to the #performance channel.

Alternatively, to do the analysis yourself, place mesos-master.stacks into the `examples` folder of a flamescope git checkout.
