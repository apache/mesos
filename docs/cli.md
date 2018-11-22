---
title: Apache Mesos - CLI
layout: documentation
---

# The new CLI

The new Mesos Command Line Interface provides one executable Python 3
script to run all default commands and additional custom plugins.

Two of the subcommands available allow you to debug running containers:

* `mesos task exec`, to run a command in a running task's container.
* `mesos task attach`, to attach your local terminal to a running task
  and stream its input/output.

## Building the CLI

For now, the Mesos CLI is still under development and not built as part
of a standard Mesos distribution.

However, the CLI can be built using [Autotools](configuration/autotools.md) and
[Cmake options](configuration/cmake.md). If necessary, check the options
described in the linked pages to set Python 3 before starting a build.

The result of this build will be a `mesos` binary that can be executed.

## Using the CLI

Using the CLI without building Mesos is also possible. To do so, activate
the CLI virtual environment by following the steps described below:

```
$ cd src/python/cli_new/
$ PYTHON=python3 ./bootstrap
$ source activate
$ mesos
```

Calling `mesos` will then run the CLI and calling `mesos-cli-tests` will
run the integration tests.

##  Configuring the CLI

The CLI uses a configuration file to know where the masters of the cluster are
as well as list any plugins that should be used in addition to the default ones
provided.

The configuation file, located by default at `~/.mesos/config.toml`, looks
like this:

```
# The `plugins` array lists the absolute paths of the
# plugins you want to add to the CLI.
plugins = [
  "</absolute/path/to/plugin-1/directory>",
  "</absolute/path/to/plugin-2/directory>"
]

# The `master` field is either composed of an `address` field
# or a `zookeeper` field, but not both. For example:
[master]
  address = "10.10.0.30:5050"
  # The `zookeeper` field has an `addresses` array and a `path` field.
  # [master.zookeeper]
  #   addresses = [
  #     "10.10.0.31:5050",
  #     "10.10.0.32:5050",
  #     "10.10.0.33:5050"
  #   ]
  #   path = "/mesos"
```
