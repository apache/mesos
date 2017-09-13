---
title: Apache Mesos - Docker Runtime Isolator in Mesos Containerizer
layout: documentation
---

# Docker Runtime Isolator in Mesos Containerizer

The Docker Runtime isolator is used for supporting runtime
configurations from the docker image (e.g., Entrypoint/Cmd, Env,
etc.). This isolator is tied with `--image_providers=docker`. If
`--image_providers` contains `docker`, this isolator must be used.
Otherwise, the agent will refuse to start.

To enable the Docker Runtime isolator, append `docker/runtime` to the
`--isolation` flag when starting the agent.

Currently, docker image default `Entrypoint`, `Cmd`, `Env`, and
`WorkingDir` are supported with docker runtime isolator. Users can
specify `CommandInfo` to override the default `Entrypoint` and `Cmd`
in the image (see below for details). The `CommandInfo` should be
inside of either `TaskInfo` or `ExecutorInfo` (depending on whether
the task is a command task or uses a custom executor, respectively).

## Determine the Launch Command

If the user specifies a command in `CommandInfo`, that will override
the default Entrypoint/Cmd in the docker image. Otherwise, we will use
the default Entrypoint/Cmd and append arguments specified in
`CommandInfo` accordingly. The details are explained in the following
table.

Users can specify `CommandInfo` including `shell`, `value` and
`arguments`, which are represented in the first column of the table
below. `0` represents `not specified`, while `1` represents
`specified`. The first row is how `Entrypoint` and `Cmd` defined in
the docker image. All cells in the table, except the first column and
row, as well as cells labeled as `Error`, have the first element
(i.e., `/Entrypt[0]`) as executable, and the rest as appending
arguments.

<table class="table table-striped">
  <tr>
    <th></th>
    <th>Entrypoint=0<br>Cmd=0</th>
    <th>Entrypoint=0<br>Cmd=1</th>
    <th>Entrypoint=1<br>Cmd=0</th>
    <th>Entrypoint=1<br>Cmd=1</th>
  </tr>
  <tr>
    <td>sh=0<br>value=0<br>argv=0</td>
    <td>Error</td>
    <td>/Cmd[0]<br>Cmd[1]..</td>
    <td>/Entrypt[0]<br>Entrypt[1]..</td>
    <td>/Entrypt[0]<br>Entrypt[1]..<br>Cmd..</td>
  </tr>
  <tr>
    <td>sh=0<br>value=0<br>argv=1</td>
    <td>Error</td>
    <td>/Cmd[0]<br>argv</td>
    <td>/Entrypt[0]<br>Entrypt[1]..<br>argv</td>
    <td>/Entrypt[0]<br>Entrypt[1]..<br>argv</td>
  </tr>
  <tr>
    <td>sh=0<br>value=1<br>argv=0</td>
    <td>/value</td>
    <td>/value</td>
    <td>/value</td>
    <td>/value</td>
  </tr>
  <tr>
    <td>sh=0<br>value=1<br>argv=1</td>
    <td>/value<br>argv</td>
    <td>/value<br>argv</td>
    <td>/value<br>argv</td>
    <td>/value<br>argv</td>
  </tr>
  <tr>
    <td>sh=1<br>value=0<br>argv=0</td>
    <td>Error</td>
    <td>Error</td>
    <td>Error</td>
    <td>Error</td>
  </tr>
  <tr>
    <td>sh=1<br>value=0<br>argv=1</td>
    <td>Error</td>
    <td>Error</td>
    <td>Error</td>
    <td>Error</td>
  </tr>
  <tr>
    <td>sh=1<br>value=1<br>argv=0</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
  </tr>
  <tr>
    <td>sh=1<br>value=1<br>argv=1</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
  </tr>
</table>
