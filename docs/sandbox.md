---
title: Apache Mesos - Sandbox
layout: documentation
---

# Mesos "Sandbox"

Mesos refers to the "sandbox" as a temporary directory that holds files specific
to a single executor.  Each time an executor is run, the executor is given its
own sandbox and the executor's working directory is set to the sandbox.

## Sandbox files

The sandbox holds:

* Files [fetched by Mesos](fetcher.md), prior to starting
  the executor's tasks.
* The output of the executor and tasks (as files "stdout" and "stderr").
* Files created by the executor and tasks, with some exceptions.

**NOTE:** With the introduction of [persistent volumes](persistent-volume.md),
executors and tasks should never create files outside of the sandbox.  However,
some containerizers do not enforce this sandboxing.

## <a name="where-is-it"></a>Where is the sandbox?

The sandbox is located within the agent's working directory (which is specified
via the `--work_dir` flag).  To find a particular executor's sandbox, you must
know the agent's ID, the executor's framework's ID, and the executor's ID.
Each run of the executor will have a corresponding sandbox, denoted by a
container ID.

The sandbox is located on the agent, inside a directory tree like the following:

```
root ('--work_dir')
|-- slaves
|   |-- latest (symlink)
|   |-- <agent ID>
|       |-- frameworks
|           |-- <framework ID>
|               |-- executors
|                   |-- <executor ID>
|                       |-- runs
|                           |-- latest (symlink)
|                           |-- <container ID> (Sandbox!)
```

## Using the sandbox

**NOTE:** For anything other than Mesos, the executor, or the task(s), the
sandbox should be considered a read-only directory.  This is not enforced via
permissions, but the executor/tasks may malfunction if the sandbox is mutated
unexpectedly.

### Via a file browser

If you have access to the machine running the agent, you can [navigate to the
sandbox directory directly](#where-is-it).

### Via the Mesos web UI

Sandboxes can be browsed and downloaded via the Mesos web UI.  Tasks and
executors will be shown with a "Sandbox" link.  Any files that live in the
sandbox will appear in the web UI.

### Via the `/files` endpoint

Underneath the web UI, the files are fetched from the agent via the `/files`
endpoint running on the agent.

<table class="table table-striped">
  <thead>
    <tr>
      <th width="30%">
        Endpoint
      </th>
      <th>
        Description
      </th>
    </tr>
  </thead>

  <tr>
    <td>
       <code>/files/browse?path=...</code>
    </td>
    <td>
      Returns a JSON list of files and directories contained in the path.
      Each list is a JSON object containing all the fields normally found in
      <code>ls -l</code>.
    </td>
  </tr>
  <tr>
    <td>
       <code>/files/debug</code>
    </td>
    <td>
      Returns a JSON object holding the internal mapping of files managed by
      this endpoint.  This endpoint can be used to quickly fetch the paths
      of all files exposed on the agent.
    </td>
  </tr>
  <tr>
    <td>
       <code>/files/download?path=...</code>
    </td>
    <td>
      Returns the raw contents of the file located at the given path.
      Where the file extension is understood, the <code>Content-Type</code>
      header will be set appropriately.
    </td>
  </tr>
  <tr>
    <td>
       <code>/files/read?path=...</code>
    </td>
    <td>
      Reads a chunk of the file located at the given path and returns a JSON
      object containing the read <code>"data"</code> and the
      <code>"offset"</code> in bytes.
      <blockquote>
        <p>
          <strong>NOTE:</strong> This endpoint is not designed to read
          arbitrary binary files. Binary files may be returned as
          invalid/un-parseable JSON.
          Use <code>/files/download</code> instead.
        </p>
      </blockquote>
      Optional query parameters:
      <ul>
        <li><code>offset</code> - can be used to page through the file.</li>
        <li><code>length</code> - maximum size of the chunk to read.</li>
      </ul>
    </td>
  </tr>
</table>

## Sandbox size

The maximum size of the sandbox is dependent on the containerization of the
executor and isolators:

* Mesos containerizer - For backwards compatibility, the Mesos containerizer
  does not enforce a container's disk quota by default.  However, if the
  `--enforce_container_disk_quota` flag is enabled on the agent, and
  `disk/du` is specified in the `--isolation` flag, the executor
  will be killed if the sandbox size exceeds the executor's `disk` resource.
* Docker containerizer - As of Docker `1.9.1`, the Docker containerizer
  does not enforce nor support a disk quota.  See the
  [Docker issue](https://github.com/docker/docker/issues/3804).

## Sandbox lifecycle

Sandbox files are scheduled for garbage collection when:

* An executor is removed or terminated.
* A framework is removed.
* An executor is recovered unsuccessfully during agent recovery.
* If the `--gc_non_executor_container_sandboxes` agent flag is enabled,
  nested container sandboxes will also be garbage collected when the
  container exits.

**NOTE:** During agent recovery, all of the executor's runs, except for the
latest run, are scheduled for garbage collection as well.

Garbage collection is scheduled based on the `--gc_delay` agent flag.  By
default, this is one week since the sandbox was last modified.
After the delay, the files are deleted.

Additionally, according to the `--disk_watch_interval` agent flag, files
scheduled for garbage collection are pruned based on the available disk and
the `--gc_disk_headroom` agent flag.
See [the formula here](configuration/agent.md#gc_disk_headroom).
