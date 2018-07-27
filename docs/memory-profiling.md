---
title: Apache Mesos - Memory Profiling
layout: documentation
---

# Memory Profiling with Mesos and Jemalloc

On Linux systems, Mesos is able to leverage the memory-profiling capabilities of
the [jemalloc](http://jemalloc.net) general-purpose allocator to provide
powerful debugging tools for investigating memory-related issues.

These include detailed real-time statistics of the current memory usage, as well
as information about the location and frequency of individual allocations.

This generally works by having libprocess detect at runtime whether the current
process is using jemalloc as its memory allocator, and if so enable a number of
HTTP endpoints described below that allow operators to generate the desired data
at runtime.


<a name="requirements"></a>
## Requirements

A prerequisite for memory profiling is a suitable allocator. Currently only
jemalloc is supported, which can be connected via one of the following ways.

The recommended method is to specify the `--enable-jemalloc-allocator`
compile-time flag, which causes the `mesos-master` and `mesos-agent` binaries
to be statically linked against a bundled version of jemalloc that will be
compiled with the correct compile-time flags.

Alternatively and analogous to other bundled dependencies of Mesos, it is of
course also possible to use a _suitable_ custom version of jemalloc with the
`--with-jemalloc=</path-to-jemalloc>` flag.

**NOTE:** Suitable here means that jemalloc should have been built with the
`--enable-stats` and `--enable-prof` flags, and that the string
`prof:true;prof_active:false` is part of the malloc configuration. The latter
condition can be satisfied either at configuration or at run-time, see the
section on `MALLOC_CONF` below.

The third way is to use the `LD_PRELOAD` mechanism to preload a `libjemalloc.so`
shared library that is present on the system at runtime. The `MemoryProfiler`
class in libprocess will automatically detect this and enable its memory
profiling support.

The generated profile dumps will be written to a random directory under `TMPDIR`
if set, otherwise in a subdirectory of `/tmp`.

Finally, note that since jemalloc was designed to be used in highly concurrent
allocation scenarios, it can improve performance over the default system
allocator. In this case, it can be beneficial to build Mesos with jemalloc even
if there is no intention to use the memory profiling functionality.

## Usage

There are two independent sets of data that can be collected from jemalloc:
memory statistics and heap profiling information.

Using any of the endpoints described below
[requires the jemalloc allocator](#requirements) and starting the `mesos-agent`
or `mesos-master` binary with the option `--memory_profiling=true` (or setting
the environment variable `LIBPROCESS_MEMORY_PROFILING=true` for other binaries
using libprocess).


### Memory Statistics

The `/statistics` endpoint returns exact statistics about the memory usage in
JSON format, for example the number of bytes currently allocated and the size
distribution of these allocations.

It takes no parameters and will return the results in JSON format:

    http://example.org:5050/memory-profiler/statistics

Be aware that the returned JSON is quite large, so when accessing this endpoint
from a terminal, it is advisable to redirect the results into a file.


### Heap Profiling

The profiling done by jemalloc works by sampling from the calls to `malloc()`
according to a configured probability distribution, and storing stack traces for
the sampled calls in a separate memory area. These can then be dumped into files
on the filesystem, so-called heap profiles.

To start a profiling run one would access the `/start` endpoint:

    http://example.org:5050/memory-profiler/start?duration=5mins

followed by downloading one of the generated files described below after the
duration has elapsed. The remaining time of the current profiling run can be
verified via the `/state` endpoint:

    http://example.org:5050/memory-profiler/state

Since profiling information is stored process-global by jemalloc, only a single
concurrent profiling run is allowed. Additionally, only the results of the most
recently finished run are stored on disk.

The profile collection can also be stopped early with the `/stop` endpoint:

    http://example.org:5050/memory-profiler/stop

To analyze the generated profiling data, the results are offered in three
different formats.

#### Raw profile

    http://example.org:5050/memory-profiler/download/raw

This returns a file in a plain text format containing the raw backtraces
collected, i.e., lists of memory addresses. It can be interactively analyzed
and rendered using the `jeprof` tool provided by the jemalloc project. For more
information on this file format, check out [the official jemalloc
documentation](http://jemalloc.net/jemalloc.3.html#heap_profile_format).

#### Symbolized profile

    http://example.org:5050/memory-profiler/download/text

This is similar to the raw format above, except that `jeprof` is called on the
host machine to attempt to read symbol information from the current binary and
replace raw memory addresses in the profile by human-readable symbol names.

Usage of this endpoint requires that `jeprof` is present on the host machine
and on the `PATH`, and no useful information will be generated unless the binary
contains symbol information.

#### Call graph

    http://example.org:5050/memory-profiler/download/graph

This endpoint returns an image in SVG format that shows a graphical
representation of the samples backtraces.

Usage of this endpoint requires that `jeprof` and `dot` are present on the host
machine and on the `PATH` of mesos, and no useful information will be generated
unless the binary contains symbol information.

#### Overview

Which of these is needed will depend on the circumstances of the application
deployment and of the bug that is investigated.

For example, the call graph presents information in a visual, immediately useful
form, but is difficult to filter and post-process if non-default output options
are desired.

On the other hand, in many debian-like environments symbol information is by
default stripped from binaries to save space and shipped in separate packages.
In such an environment, if it is not permitted to install additional packages on
the host running Mesos, one would store the raw profiles and enrich them with
symbol information locally.


## Jeprof Installation

As described above, the `/download/text` and `/download/graph` endpoints require
the `jeprof` program installed on the host system. Where possible, it is
recommended to install `jeprof` through the system package manager, where it is
usually packaged alongside with jemalloc itself.

Alternatively, a copy of the script can be found under
`3rdparty/jemalloc-5.0.1/bin/jeprof` in the build directory, or can be
downloaded directly from the internet using a command like:

    $ curl https://raw.githubusercontent.com/jemalloc/jemalloc/dev/bin/jeprof.in | sed s/@jemalloc_version@/5.0.1/ >jeprof

Note that `jeprof` is just a perl script that post-processes the raw profiles.
It has no connection to the jemalloc library besides being distributed in the
same package. In particular, it is generally not required to have matching
versions of jemalloc and `jeprof`.

If `jeprof` is installed manually, one also needs to take care to install the
necessary dependencies. In particular, this include the `perl` interpreter to
execute the script itself and the `dot` binary to generate graph files.


## Command-line Usage

In some circumstances, it might be desired to automate the downloading of heap
profiles by writing a simple script. A simple example for how this might look
like this:

    #!/bin/bash

    SECONDS=600
    HOST=example.org:5050

    curl ${HOST}/memory-profiler/start?duration=${SECONDS}
    sleep $((${SECONDS} + 1))
    wget ${HOST}/memory-profiler/download/raw

A more sophisticated script would additionally store the `id` value returned by
the call to `/start` and pass it as a paremter to `/download`, to ensure that a
new run was not started in the meantime.


## Using the `MALLOC_CONF` Interface

The jemalloc allocator provides a native interface to control the memory
profiling behaviour. The usual way to provide settings through this interface is
by setting the environment variable `MALLOC_CONF`.

**NOTE:** If libprocess detects that memory profiling was started through
`MALLOC_CONF`, it will reject starting a profiling run of its own to avoid
interference.

The `MALLOC_CONF` interface provides a number of options that are not exposed by
libprocess, like generating heap profiles automatically after a certain amount
of memory has been allocated, or whenever memory usage reaches a new high-water
mark. The full list of settings is described on the
[jemalloc man page](http://jemalloc.net/jemalloc.3.html).

On the other hand, features like starting and stopping the profiling at runtime
or getting the information provided by the `/statistics` endpoint can not be
achieved through the `MALLOC_CONF` interface.

For example, to create a dump automatically for every 1 GiB worth of recorded
allocations, one might use the configuration:

    MALLOC_CONF="prof:true,prof_prefix:/path/to/folder,lg_prof_interval=20"

To debug memory allocations during early startup, profiling can be activated
before accessing the `/start` endpoint:

    MALLOC_CONF="prof:true,prof_active:true"
