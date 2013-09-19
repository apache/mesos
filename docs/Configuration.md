# Configuration #
If you have special compilation requirements, please refer to `./configure --help` when configuring Mesos.

The Mesos master and slave can take a variety of configuration options through command-line arguments, or environment variables. A list of the available options can be seen by running `mesos-master --help` or `mesos-slave --help`. Each option can be set in three ways:

  - By passing it to the binary using `--option_name=value`.
  - By setting the environment variable `MESOS_OPTION_NAME` (the option name with a `MESOS_` prefix added to it).

Configuration values are searched for first in the environment, then on the command-line.

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

  - `roles` : A comma seperated list of the allocation roles
             that frameworks in this cluster may belong to.

  - `weights` : A comma seperated list of role/weight pairs
                of the form 'role=weight,role=weight'. Weights
                are used to indicate forms of priority.

**For the complete list of master options: ./mesos-master.sh --help**

### Slave Options ###

  - `resources` : Total consumable resources per slave, in
                  the form 'name(role):value;name(role):value...'.
    - NOTE: '(role)' is optional.
    - Ex: "cpus(role2):2;mem(role2):1024;cpus:1;mem:1024;disk:0"

  - `attributes` : Attributes for the machine. [Optional]
    - These are free-form in the same style as resources, ex: "rack:abc;kernel:2.6.44".
    - This information is provided to frameworks.

  - `work_dir` : Directory for the executor work directories. [Default: /tmp/mesos]

  - `isolation` : Isolation mechanism, one of: "process", "cgroups". [Default: process]

  - `cgroups_enable_cfs` : If using cgroups isolation, this enables hard limits on CPU resources.

  - `master` : May be one of:
    - zk://host1:port1,host2:port2,.../path
    - zk://username:password@host1:port1,host2:port2,.../path\n
    - file://path/to/file (where file contains one of the above)

  - `default_role` : Any resources in the --resources flag that
                     omit a role, as well as any resources that
                     are not present in --resources but that are
                     automatically detected, will be assigned to
                     this role. [Default: *]

  - `checkpoint` :  Whether to checkpoint slave and frameworks information
                    to disk.
    - This enables a restarted slave to recover status updates and reconnect
      with (--recover=reconnect) or kill (--recover=kill) old executors [Default: false]

  - `strict` : Whether to do recovery in strict mode [Default: true].
    - If strict=true, any and all recovery errors are considered fatal.
    - If strict=false, any errors (e.g., corruption in checkpointed data) during recovery are
      ignored and as much state as possible is recovered.

**For the complete list of slave options: ./mesos-slave.sh --help**
