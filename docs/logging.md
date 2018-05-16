---
title: Apache Mesos - Logging
layout: documentation
---

# Logging

Mesos handles the logs of each Mesos component differently depending on the
degree of control Mesos has over the source code of the component.

Roughly, these categories are:

* [Internal](#Internal) - Master and Agent.
* [Containers](#Containers) - Executors and Tasks.
* External - Components launched outside of Mesos, like
  Frameworks and [ZooKeeper](high-availability.md).  These are expected to
  implement their own logging solution.

## <a name="Internal"></a>Internal

The Mesos Master and Agent use the
[Google's logging library](https://github.com/google/glog).
For information regarding the command-line options used to configure this
library, see the
[configuration documentation](configuration/master-and-agent.md#logging-options).
Google logging options that are not explicitly mentioned there can be
configured via environment variables.

Both Master and Agent also expose a [/logging/toggle](endpoints/logging/toggle.md)
HTTP endpoint which temporarily toggles verbose logging:

```
POST <ip:port>/logging/toggle?level=[1|2|3]&duration=VALUE
```

The effect is analogous to setting the `GLOG_v` environment variable prior
to starting the Master/Agent, except the logging level will revert to the
original level after the given duration.

## <a name="Containers"></a>Containers

For background, see [the containerizer documentation](containerizers.md).

Mesos does not assume any structured logging for entities running inside
containers.  Instead, Mesos will store the stdout and stderr of containers
into plain files ("stdout" and "stderr") located inside
[the sandbox](sandbox.md#where-is-it).

In some cases, the default Container logger behavior of Mesos is not ideal:

* Logging may not be standardized across containers.
* Logs are not easily aggregated.
* Log file sizes are not managed.  Given enough time, the "stdout" and "stderr"
  files can fill up the Agent's disk.

## `ContainerLogger` Module

The `ContainerLogger` module was introduced in Mesos 0.27.0 and aims to address
the shortcomings of the default logging behavior for containers.  The module
can be used to change how Mesos redirects the stdout and stderr of containers.

The [interface for a `ContainerLogger` can be found here](https://github.com/apache/mesos/blob/master/include/mesos/slave/container_logger.hpp).

Mesos comes with two `ContainerLogger` modules:

* The `SandboxContainerLogger` implements the existing logging behavior as
  a `ContainerLogger`.  This is the default behavior.
* The `LogrotateContainerLogger` addresses the problem of unbounded log file
  sizes.

### `LogrotateContainerLogger`

The `LogrotateContainerLogger` constrains the total size of a container's
stdout and stderr files.  The module does this by rotating log files based
on the parameters to the module.  When a log file reaches its specified
maximum size, it is renamed by appending a `.N` to the end of the filename,
where `N` increments each rotation.  Older log files are deleted when the
specified maximum number of files is reached.

#### Invoking the module

The `LogrotateContainerLogger` can be loaded by specifying the library
`liblogrotate_container_logger.so` in the
[`--modules` flag](modules.md#Invoking) when starting the Agent and by
setting the `--container_logger` Agent flag to
`org_apache_mesos_LogrotateContainerLogger`.

#### Module parameters

<table class="table table-striped">
  <thead>
    <tr>
      <th width="30%">
        Key
      </th>
      <th>
        Explanation
      </th>
    </tr>
  </thead>

  <tr>
    <td>
      <code>max_stdout_size</code>/<code>max_stderr_size</code>
    </td>
    <td>
      Maximum size, in bytes, of a single stdout/stderr log file.
      When the size is reached, the file will be rotated.

      Defaults to 10 MB.  Minimum size of 1 (memory) page, usually around 4 KB.
    </td>
  </tr>

  <tr>
    <td>
      <code>logrotate_stdout_options</code>/
      <code>logrotate_stderr_options</code>
    </td>
    <td>
      Additional config options to pass into <code>logrotate</code> for stdout.
      This string will be inserted into a <code>logrotate</code> configuration
      file. i.e. For "stdout":
      <pre>
/path/to/stdout {
  [logrotate_stdout_options]
  size [max_stdout_size]
}</pre>
      NOTE: The <code>size</code> option will be overridden by this module.
    </td>
  </tr>

  <tr>
    <td>
      <code>environment_variable_prefix</code>
    </td>
    <td>
      Prefix for environment variables meant to modify the behavior of
      the logrotate logger for the specific container being launched.
      The logger will look for four prefixed environment variables in the
      container's <code>CommandInfo</code>'s <code>Environment</code>:
      <ul>
        <li><code>MAX_STDOUT_SIZE</code></li>
        <li><code>LOGROTATE_STDOUT_OPTIONS</code></li>
        <li><code>MAX_STDERR_SIZE</code></li>
        <li><code>LOGROTATE_STDERR_OPTIONS</code></li>
      </ul>
      If present, these variables will overwrite the global values set
      via module parameters.

      Defaults to <code>CONTAINER_LOGGER_</code>.
    </td>
  </tr>

  <tr>
    <td>
      <code>launcher_dir</code>
    </td>
    <td>
      Directory path of Mesos binaries.
      The <code>LogrotateContainerLogger</code> will find the
      <code>mesos-logrotate-logger</code> binary under this directory.

      Defaults to <code>/usr/local/libexec/mesos</code>.
    </td>
  </tr>

  <tr>
    <td>
      <code>logrotate_path</code>
    </td>
    <td>
      If specified, the <code>LogrotateContainerLogger</code> will use the
      specified <code>logrotate</code> instead of the system's
      <code>logrotate</code>.  If <code>logrotate</code> is not found, then
      the module will exit with an error.
    </td>
  </tr>
</table>

#### How it works

1. Every time a container starts up, the `LogrotateContainerLogger`
   starts up companion subprocesses of the `mesos-logrotate-logger` binary.
2. The module instructs Mesos to redirect the container's stdout/stderr
   to the `mesos-logrotate-logger`.
3. As the container outputs to stdout/stderr, `mesos-logrotate-logger` will
   pipe the output into the "stdout"/"stderr" files.  As the files grow,
   `mesos-logrotate-logger` will call `logrotate` to keep the files strictly
   under the configured maximum size.
4. When the container exits, `mesos-logrotate-logger` will finish logging before
   exiting as well.

The `LogrotateContainerLogger` is designed to be resilient across Agent
failover.  If the Agent process dies, any instances of `mesos-logrotate-logger`
will continue to run.

### Writing a Custom `ContainerLogger`

For basics on module writing, see [the modules documentation](modules.md).

There are several caveats to consider when designing a new `ContainerLogger`:

* Logging by the `ContainerLogger` should be resilient to Agent failover.
  If the Agent process dies (which includes the `ContainerLogger` module),
  logging should continue.  This is usually achieved by using subprocesses.
* When containers shut down, the `ContainerLogger` is not explicitly notified.
  Instead, encountering `EOF` in the container's stdout/stderr signifies
  that the container has exited.  This provides a stronger guarantee that the
  `ContainerLogger` has seen all the logs before exiting itself.
* The `ContainerLogger` should not assume that containers have been launched
  with any specific `ContainerLogger`.  The Agent may be restarted with a
  different `ContainerLogger`.
* Each [containerizer](containerizers.md) running on an Agent uses its own
  instance of the `ContainerLogger`.  This means more than one `ContainerLogger`
  may be running in a single Agent.  However, each Agent will only run a single
  type of `ContainerLogger`.
