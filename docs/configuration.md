---
title: Apache Mesos - Configuration
layout: documentation
---

# Mesos Runtime Configuration

The Mesos master and agent can take a variety of configuration options
through command-line arguments or environment variables. A list of the
available options can be seen by running `mesos-master --help` or
`mesos-agent --help`. Each option can be set in two ways:

* By passing it to the binary using `--option_name=value`, either
specifying the value directly, or specifying a file in which the value
resides (`--option_name=file://path/to/file`). The path can be
absolute or relative to the current working directory.

* By setting the environment variable `MESOS_OPTION_NAME` (the option
name with a `MESOS_` prefix added to it).

Configuration values are searched for first in the environment, then
on the command-line.

Additionally, this documentation lists only a recent snapshot of the options in
Mesos. A definitive source for which flags your version of Mesos supports can be
found by running the binary with the flag `--help`, for example `mesos-master
--help`.

## Master and Agent Options

*These are options common to both the Mesos master and agent.*

See [configuration/master-and-agent.md](configuration/master-and-agent.md).

## Master Options

See [configuration/master.md](configuration/master.md).

## Agent Options

See [configuration/agent.md](configuration/agent.md).

## Libprocess Options

See [configuration/libprocess.md](configuration/libprocess.md).

# Mesos Build Configuration

## Autotools Options

If you have special compilation requirements, please refer to `./configure
--help` when configuring Mesos.

See [configuration/autotools.md](configuration/autotools.md).

## CMake Options

See [configuration/cmake.md](configuration/cmake.md).
