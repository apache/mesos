---
layout: documentation
---

# Mesos Configuration

The Mesos master and slave can take a variety of configuration options through command-line arguments, or environment variables. A list of the available options can be seen by running `mesos-master --help` or `mesos-slave --help`. Each option can be set in two ways:

* By passing it to the binary using `--option_name=value`.
* By setting the environment variable `MESOS_OPTION_NAME` (the option name with a `MESOS_` prefix added to it).

Configuration values are searched for first in the environment, then on the command-line.

**Important Options**

If you have special compilation requirements, please refer to `./configure --help` when configuring Mesos. Additionally, the documentation lists only a subset of the options. A definitive source for which flags your version of Mesos supports can be found by running the binary with the flag `--help`, for example `mesos-master --help`.

## Master and Slave Options

*These options can be supplied to both masters and slaves.*

```
  --ip=VALUE                                 IP address to listen on

  --[no-]help                                Prints this help message (default: false)

  --log_dir=VALUE                            Location to put log files (no default, nothing
                                             is written to disk unless specified;
                                             does not affect logging to stderr)

  --logbufsecs=VALUE                         How many seconds to buffer log messages for (default: 0)

  --logging_level=VALUE                      Log message at or above this level; possible values:
                                             'INFO', 'WARNING', 'ERROR'; if quiet flag is used, this
                                             will affect just the logs from log_dir (if specified) (default: INFO)

  --port=VALUE                               Port to listen on (default: 5051)

  --[no-]quiet                               Disable logging to stderr (default: false)

  --[no-]version                             Show version and exit. (default: false)
```

## Master Options

*Required Flags*

```
  --quorum=VALUE                           The size of the quorum of replicas when using 'replicated_log' based
                                           registry. It is imperative to set this value to be a majority of
                                           masters i.e., quorum > (number of masters)/2.

  --work_dir=VALUE                         Where to store the persistent information stored in the Registry.

  --zk=VALUE                               ZooKeeper URL (used for leader election amongst masters)
                                           May be one of:
                                             zk://host1:port1,host2:port2,.../path
                                             zk://username:password@host1:port1,host2:port2,.../path
                                             file://path/to/file (where file contains one of the above)
```

*Optional Flags*

```
  --allocation_interval=VALUE              Amount of time to wait between performing
                                            (batch) allocations (e.g., 500ms, 1sec, etc). (default: 1secs)
  --[no-]authenticate                      If authenticate is 'true' only authenticated frameworks are allowed
                                           to register. If 'false' unauthenticated frameworks are also
                                           allowed to register. (default: false)
  --[no-]authenticate_slaves               If 'true' only authenticated slaves are allowed to register.
                                           If 'false' unauthenticated slaves are also allowed to register. (default: false)
  --cluster=VALUE                          Human readable name for the cluster,
                                           displayed in the webui.
  --credentials=VALUE                      Path to a file with a list of credentials.
                                           Each line contains 'principal' and 'secret' separated by whitespace.
                                           Path could be of the form 'file:///path/to/file' or '/path/to/file'.
  --framework_sorter=VALUE                 Policy to use for allocating resources
                                           between a given user's frameworks. Options
                                           are the same as for user_allocator. (default: drf)
  --hostname=VALUE                         The hostname the master should advertise in ZooKeeper.
                                           If left unset, system hostname will be used (recommended).
  --[no-]log_auto_initialize               Whether to automatically initialize the replicated log used for the
                                           registry. If this is set to false, the log has to be manually
                                           initialized when used for the very first time. (default: true)

  --recovery_slave_removal_limit=VALUE     For failovers, limit on the percentage of slaves that can be removed
                                           from the registry *and* shutdown after the re-registration timeout
                                           elapses. If the limit is exceeded, the master will fail over rather
                                           than remove the slaves.
                                           This can be used to provide safety guarantees for production
                                           environments. Production environments may expect that across Master
                                           failovers, at most a certain percentage of slaves will fail
                                           permanently (e.g. due to rack-level failures).
                                           Setting this limit would ensure that a human needs to get
                                           involved should an unexpected widespread failure of slaves occur
                                           in the cluster.
                                           Values: [0%-100%] (default: 100%)

  --registry=VALUE                         Persistence strategy for the registry;
                                           available options are 'replicated_log', 'in_memory' (for testing). (default: replicated_log)

  --registry_fetch_timeout=VALUE           Duration of time to wait in order to fetch data from the registry
                                           after which the operation is considered a failure. (default: 1mins)

  --registry_store_timeout=VALUE           Duration of time to wait in order to store data in the registry
                                           after which the operation is considered a failure. (default: 5secs)

  --[no-]registry_strict                   Whether the Master will take actions based on the persistent
                                           information stored in the Registry. Setting this to false means
                                           that the Registrar will never reject the admission, readmission,
                                           or removal of a slave. Consequently, 'false' can be used to
                                           bootstrap the persistent state on a running cluster.
                                           NOTE: This flag is *experimental* and should not be used in
                                           production yet. (default: false)

  --roles=VALUE                            A comma seperated list of the allocation
                                           roles that frameworks in this cluster may
                                           belong to.

  --[no-]root_submissions                  Can root submit frameworks? (default: true)

  --slave_reregister_timeout=VALUE         The timeout within which all slaves are expected to re-register
                                           when a new master is elected as the leader. Slaves that do not
                                           re-register within the timeout will be removed from the registry
                                           and will be shutdown if they attempt to communicate with master.
                                           NOTE: This value has to be atleast 10mins. (default: 10mins)

  --user_sorter=VALUE                      Policy to use for allocating resources
                                           between users. May be one of:
                                             dominant_resource_fairness (drf) (default: drf)

  --webui_dir=VALUE                        Location of the webui files/assets (default: /usr/local/share/mesos/webui)

  --weights=VALUE                          A comma seperated list of role/weight pairs
                                           of the form 'role=weight,role=weight'. Weights
                                           are used to indicate forms of priority.

  --whitelist=VALUE                        Path to a file with a list of slaves
                                           (one per line) to advertise offers for.
                                           Path could be of the form 'file:///path/to/file' or '/path/to/file'. (default: *)

  --zk_session_timeout=VALUE               ZooKeeper session timeout. (default: 10secs)
```

## Slave Options

*Required Flags*

```
  --master=VALUE                             May be one of:
                                               zk://host1:port1,host2:port2,.../path
                                               zk://username:password@host1:port1,host2:port2,.../path
                                               file://path/to/file (where file contains one of the above)
```

*Optional Flags*

```
  --attributes=VALUE                         Attributes of machine

  --[no-]cgroups_enable_cfs                  Cgroups feature flag to enable hard limits on CPU resources
                                             via the CFS bandwidth limiting subfeature.
                                             (default: false)

  --cgroups_hierarchy=VALUE                  The path to the cgroups hierarchy root
                                             (default: /sys/fs/cgroup)

  --cgroups_root=VALUE                       Name of the root cgroup
                                             (default: mesos)

  --cgroups_subsystems=VALUE                 This flag has been deprecated and is no longer used,
                                             please update your flags

  --[no-]checkpoint                          Whether to checkpoint slave and frameworks information
                                             to disk. This enables a restarted slave to recover
                                             status updates and reconnect with (--recover=reconnect) or
                                             kill (--recover=cleanup) old executors (default: true)

  --containerizer_path=VALUE                 The path to the external containerizer executable used when
                                             external isolation is activated (--isolation=external).

  --credential=VALUE                         Path to a file containing a single line with
                                             the 'principal' and 'secret' separated by whitespace.
                                             Path could be of the form 'file:///path/to/file' or '/path/to/file'

  --default_container_image=VALUE            The default container image to use if not specified by a task,
                                             when using external containerizer

  --default_role=VALUE                       Any resources in the --resources flag that
                                             omit a role, as well as any resources that
                                             are not present in --resources but that are
                                             automatically detected, will be assigned to
                                             this role. (default: *)

  --disk_watch_interval=VALUE                Periodic time interval (e.g., 10secs, 2mins, etc)
                                             to check the disk usage (default: 1mins)

  --executor_registration_timeout=VALUE      Amount of time to wait for an executor
                                             to register with the slave before considering it hung and
                                             shutting it down (e.g., 60secs, 3mins, etc) (default: 1mins)

  --executor_shutdown_grace_period=VALUE     Amount of time to wait for an executor
                                             to shut down (e.g., 60secs, 3mins, etc) (default: 5secs)

  --frameworks_home=VALUE                    Directory prepended to relative executor URIs (default: )

  --gc_delay=VALUE                           Maximum amount of time to wait before cleaning up
                                             executor directories (e.g., 3days, 2weeks, etc).
                                             Note that this delay may be shorter depending on
                                             the available disk usage. (default: 1weeks)

  --hadoop_home=VALUE                        Where to find Hadoop installed (for
                                             fetching framework executors from HDFS)
                                             (no default, look for HADOOP_HOME in
                                             environment or find hadoop on PATH) (default: )

  --hostname=VALUE                           The hostname the slave should report.
                                             If left unset, system hostname will be used (recommended).

  --isolation=VALUE                          Isolation mechanisms to use, e.g., 'posix/cpu,posix/mem'
                                             or 'cgroups/cpu,cgroups/mem' or 'external'. (default: posix/cpu,posix/mem)

  --launcher_dir=VALUE                       Location of Mesos binaries (default: /usr/local/libexec/mesos)

  --recover=VALUE                            Whether to recover status updates and reconnect with old executors.
                                             Valid values for 'recover' are
                                             reconnect: Reconnect with any old live executors.
                                             cleanup  : Kill any old live executors and exit.
                                                        Use this option when doing an incompatible slave
                                                        or executor upgrade!).
                                             NOTE: If checkpointed slave doesn't exist, no recovery is performed
                                                   and the slave registers with the master as a new slave. (default: reconnect)

  --recovery_timeout=VALUE                   Amount of time alloted for the slave to recover. If the slave takes
                                             longer than recovery_timeout to recover, any executors that are
                                             waiting to reconnect to the slave will self-terminate.
                                             NOTE: This flag is only applicable when checkpoint is enabled.
                                             (default: 15mins)

  --registration_backoff_factor=VALUE        Slave initially picks a random amount of time between [0, b], where
                                             b = register_backoff_factor, to (re-)register with a new master.
                                             Subsequent retries are exponentially backed off based on this
                                             interval (e.g., 1st retry uses a random value between [0, b * 2^1],
                                             2nd retry between [0, b * 2^2], 3rd retry between [0, b * 2^3] etc)
                                             up to a maximum of 1mins (default: 1secs)

  --resource_monitoring_interval=VALUE       Periodic time interval for monitoring executor
                                             resource usage (e.g., 10secs, 1min, etc) (default: 1secs)

  --resources=VALUE                          Total consumable resources per slave, in
                                             the form 'name(role):value;name(role):value...'.

  --slave_subsystems=VALUE                   List of comma-separated cgroup subsystems to run the slave binary
                                             in, e.g., 'memory,cpuacct'. The default is none.
                                             Present functionality is intended for resource monitoring and
                                             no cgroup limits are set, they are inherited from the root mesos
                                             cgroup.

  --[no-]strict                              If strict=true, any and all recovery errors are considered fatal.
                                             If strict=false, any expected errors (e.g., slave cannot recover
                                             information about an executor, because the slave died right before
                                             the executor registered.) during recovery are ignored and as much
                                             state as possible is recovered.
                                             (default: true)

  --[no-]switch_user                         Whether to run tasks as the user who
                                             submitted them rather than the user running
                                             the slave (requires setuid permission) (default: true)

  --work_dir=VALUE                           Where to place framework work directories
                                             (default: /tmp/mesos)
```




## Mesos Build Configuration Options

The configure script has the following options:

```
To assign environment variables (e.g., CC, CFLAGS...), specify them as
VAR=VALUE.  See below for descriptions of some of the useful variables.

Defaults for the options are specified in brackets.

Configuration:
  -h, --help              display this help and exit
      --help=short        display options specific to this package
      --help=recursive    display the short help of all the included packages
  -V, --version           display version information and exit
  -q, --quiet, --silent   do not print `checking...' messages
      --cache-file=FILE   cache test results in FILE [disabled]
  -C, --config-cache      alias for `--cache-file=config.cache'
  -n, --no-create         do not create output files
      --srcdir=DIR        find the sources in DIR [configure dir or `..']

Installation directories:
  --prefix=PREFIX         install architecture-independent files in PREFIX
                          [/usr/local]
  --exec-prefix=EPREFIX   install architecture-dependent files in EPREFIX
                          [PREFIX]

By default, `make install' will install all the files in
`/usr/local/bin', `/usr/local/lib' etc.  You can specify
an installation prefix other than `/usr/local' using `--prefix',
for instance `--prefix=$HOME'.

For better control, use the options below.

Fine tuning of the installation directories:
  --bindir=DIR            user executables [EPREFIX/bin]
  --sbindir=DIR           system admin executables [EPREFIX/sbin]
  --libexecdir=DIR        program executables [EPREFIX/libexec]
  --sysconfdir=DIR        read-only single-machine data [PREFIX/etc]
  --sharedstatedir=DIR    modifiable architecture-independent data [PREFIX/com]
  --localstatedir=DIR     modifiable single-machine data [PREFIX/var]
  --libdir=DIR            object code libraries [EPREFIX/lib]
  --includedir=DIR        C header files [PREFIX/include]
  --oldincludedir=DIR     C header files for non-gcc [/usr/include]
  --datarootdir=DIR       read-only arch.-independent data root [PREFIX/share]
  --datadir=DIR           read-only architecture-independent data [DATAROOTDIR]
  --infodir=DIR           info documentation [DATAROOTDIR/info]
  --localedir=DIR         locale-dependent data [DATAROOTDIR/locale]
  --mandir=DIR            man documentation [DATAROOTDIR/man]
  --docdir=DIR            documentation root [DATAROOTDIR/doc/mesos]
  --htmldir=DIR           html documentation [DOCDIR]
  --dvidir=DIR            dvi documentation [DOCDIR]
  --pdfdir=DIR            pdf documentation [DOCDIR]
  --psdir=DIR             ps documentation [DOCDIR]

Program names:
  --program-prefix=PREFIX            prepend PREFIX to installed program names
  --program-suffix=SUFFIX            append SUFFIX to installed program names
  --program-transform-name=PROGRAM   run sed PROGRAM on installed program names

System types:
  --build=BUILD     configure for building on BUILD [guessed]
  --host=HOST       cross-compile to build programs to run on HOST [BUILD]
  --target=TARGET   configure for building compilers for TARGET [HOST]

Optional Features:
  --disable-option-checking  ignore unrecognized --enable/--with options
  --disable-FEATURE       do not include FEATURE (same as --enable-FEATURE=no)
  --enable-FEATURE[=ARG]  include FEATURE [ARG=yes]
  --enable-shared[=PKGS]  build shared libraries [default=yes]
  --enable-static[=PKGS]  build static libraries [default=yes]
  --enable-fast-install[=PKGS]
                          optimize for fast installation [default=yes]
  --disable-dependency-tracking  speeds up one-time build
  --enable-dependency-tracking   do not reject slow dependency extractors
  --disable-libtool-lock  avoid locking (might break parallel builds)
  --disable-java          don't build Java bindings
  --disable-python        don't build Python bindings
  --disable-optimize      don't try to compile with optimizations
  --disable-bundled       build against preinstalled dependencies instead of
                          bundled libraries

Optional Packages:
  --with-PACKAGE[=ARG]    use PACKAGE [ARG=yes]
  --without-PACKAGE       do not use PACKAGE (same as --with-PACKAGE=no)
  --with-pic[=PKGS]       try to use only PIC/non-PIC objects [default=use
                          both]
  --with-gnu-ld           assume the C compiler uses GNU ld [default=no]
  --with-sysroot=DIR Search for dependent libraries within DIR
                        (or the compiler's sysroot if not specified).
  --with-zookeeper[=DIR]  excludes building and using the bundled ZooKeeper
                          package in lieu of an installed version at a
                          location prefixed by the given path
  --with-leveldb[=DIR]    excludes building and using the bundled LevelDB
                          package in lieu of an installed version at a
                          location prefixed by the given path
  --without-cxx11         builds Mesos without C++11 support (deprecated)
  --with-network-isolator builds the network isolator

Some influential environment variables:
  CC          C compiler command
  CFLAGS      C compiler flags
  LDFLAGS     linker flags, e.g. -L<lib dir> if you have libraries in a
              nonstandard directory <lib dir>
  LIBS        libraries to pass to the linker, e.g. -l<library>
  CPPFLAGS    C/C++/Objective C preprocessor flags, e.g. -I<include dir> if
              you have headers in a nonstandard directory <include dir>
  CXX         C++ compiler command
  CXXFLAGS    C++ compiler flags
  CPP         C preprocessor
  CXXCPP      C++ preprocessor
  JAVA_HOME   location of Java Development Kit (JDK)
  JAVA_CPPFLAGS
              preprocessor flags for JNI
  JAVA_JVM_LIBRARY
              full path to libjvm.so
  MAVEN_HOME  looks for mvn at MAVEN_HOME/bin/mvn
  PYTHON      which Python interpreter to use
  PYTHON_VERSION
              The installed Python version to use, for example '2.3'. This
              string will be appended to the Python interpreter canonical
              name.

Use these variables to override the choices made by `configure' or to help
it to find libraries and programs with nonstandard names/locations.
```
