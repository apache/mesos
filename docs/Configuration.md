# Configuration #
If you have special compilation requirements, please refer to `./configure --help` when configuring Mesos.

The Mesos master and slave can take a variety of configuration options through command-line arguments, or environment variables. A list of the available options can be seen by running `mesos-master --help` or `mesos-slave --help`. Each option can be set in three ways:

  - By passing it to the binary using `--option_name=value`.
  - By setting the environment variable `MESOS_OPTION_NAME` (the option name in all caps with a `MESOS_` prefix added to it).

Configuration values are searched for first on the command line, then in the environment.

## Important Options ##

These are only a subset of the options. The following list may not match the version of mesos you are running, as the flags evolve from time to time in the code. 

**For now, the definitive source for which flags your version of Mesos supports can be found by running the binary with the flag `--help`, for example `bin/mesos-master --help`**. This will change as we add canonical documentation for each release.

### Master and Slave Options ###

  - `log_dir` : Directory for log files. If unspecified, nothing is written to disk.
  - `quiet` : Disable logging to stderr. [Default: false]
  - `ip` : IP address to listen on. [Optional]
  - `port` : Port to listen on. [Default for master: 5050, Default for slave:5051]

### Master Options ###

  - `zk` : ZooKeeper URL (used for leader election amongst multiple masters). May be one of:
    - zk://host1:port1,host2:port2,.../path
    - zk://username:password@host1:port1,host2:port2,.../path
    - file://path/to/file (where file contains one of the above)

### Slave Options ###

  - `resources` : Total consumable resources for the slave.
    - Ex: "cpus:8.0;memory:4096;disk:20480;ports:[30000-50000]"
  - `attributes` : Attributes for the machine. [Optional]
    - These are free-form in the same style as resources, ex: "rack:abc;kernel:2.6.44".
    - This information is provided to frameworks.
  - `work_dir` : Directory for the framework work directories. [Default: /tmp/mesos]
  - `isolation` : Isolation mechanism, one of: "process", "cgroups". [Default: process]
  - `cgroups_enable_cfs` : If using cgroups isolation, this enables hard limits on CPU resources.
  - `master` : May be one of:
    - zk://host1:port1,host2:port2,.../path
    - zk://username:password@host1:port1,host2:port2,.../path\n
    - file://path/to/file (where file contains one of the above)
