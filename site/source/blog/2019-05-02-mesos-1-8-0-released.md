---
layout: post
title: "Apache Mesos 1.8: Allocator Performance, Seccomp Isolation and Operation Feedback"
permalink: /blog/mesos-1-8-0-released/
published: true
post_author:
  display_name: Benno Evers
tags: Release
---

We're pleased to announce that Mesos 1.8.0 is now available for [download](/downloads).

As usual, lots of work and care went into this release, with a total of 255 JIRA issues resolved,
and 1149 commits containing a total diffstat of 656 files changed, 62213 insertions(+) and 20403 deletions(-).

     4.9% 3rdparty/
           1.5% 3rdparty/libprocess/
           1.6% 3rdparty/stout/
     2.3% docs/
     2.5% include/
     2.0% site/
    77.4% src/
           7.8% src/csi/
           3.6% src/examples/
          11.0% src/master/
           4.2% src/resource_provider/
          10.3% src/slave/
          28.8% src/tests/
    6.7% support/
           3.7% support/python3/


Looking at the distribution of changes, nearly all components have seen major activity.
Some of that is presented in more detail below.

One major focus for the 1.8 contributors seems to have been solid test coverage, with
nearly one third of the total amount of lines changed being inside the test suite.


## Highlights

### Allocator Performance Improvements

In Mesos 1.8, allocator cycle time is significantly decreased when quota is
used; around 40% for a small size cluster and up to 70% for larger clusters.
This greatly narrows the allocator performance gap between quota and non-quota
usage scenarios.

In addition, schedulers can now specify the minimum resource quantities needed
in an offer, which acts as an override of the global `--min_allocatable_resources`
master flag. Updating schedulers to specify this field improves multi-scheduler
scalability as it reduces the amount of offers declined from having insufficient
resource quantities.

Note that this feature currently requires that the scheduler re-subscribes each
time it wants to mutate the minimum resource quantity offer filter information,
see [MESOS-7258](https://issues.apache.org/jira/browse/MESOS-7258).


### Seccomp Isolator

A new `linux/seccomp` [isolator](docs/isolators/linux-seccomp) was added. This
isolator makes use of the
[seccomp](https://www.kernel.org/doc/Documentation/prctl/seccomp_filter.txt)
facility provided by recent linux kernels.

Using this isolator, containers launched by Mesos containerizer can be sandboxed
by enabling filtering of system calls using a configurable policy.


### Operation Feedback

In Mesos 1.6, operation feedback was introduced for operations on resources
by a resource provider.

In Mesos 1.8, v1 schedulers can now receive operation feedback for operations
on agent default resources, i.e. normal cpu, memory, and disk. This means that
the v1 scheduler API's operation feedback feature can now be used for any offer
operations except for `LAUNCH` and `LAUNCH_GROUP`, on any type of resources.


### Containerization

A number of containerization-related improvements have landed in Mesos 1.8:

  - Support pulling docker images with docker manifest
    V2 Schema2 on Mesos Containerizer.

  - Support custom port range option to the `network/ports`
    isolator. Added the `--container_ports_isolated_range` flag to the
    `network/ports` isolator. This allows the operator to specify a custom
    port range to be protected by the isolator.

  - Support XFS quota for persistent volumes. Added
    persistent volume support to the `disk/xfs` isolator.

  - Support an option to create non-existing host
    paths for host path volume in Mesos Containerizer. Added a new
    agent flag `--host_path_volume_force_creation` for the
    `volume/host_path` isolator.


### CLI Improvements

The Mesos CLI now offers the task subcommand with two actions. The first
action, `task attach`, allows you to attach your terminal to a running
task launched with a tty. The second action, `task exec`, launches a
new nested container inside a running task.

To build the CLI, use the flag `--enable-new-cli` with Autotools or
`-DENABLE_NEW_CLI=1` with CMake on MacOS or Linux.


### Experimental Features

Mesos 1.8 adds experimental support for the new CSI v1 API. Operators can deploy
plugins that are compatible to either CSI v0 or v1 to create persistent
volumes through storage local resource providers, and Mesos will
automatically detect which CSI versions are supported by the plugins.


## Upgrade

Upgrades from Mesos 1.7.0 to Mesos 1.8.0 should be straightforward. Please
refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/)
for detailed information on upgrading to Mesos 1.8.0.

- Frameworks relying on the experimental `RECONCILE_OPERATIONS` call can
  not be updated to Mesos 1.8, since the API of that has been reworked in
  a non-backwards compatible manner.


## Credits

Finally, a big **THANK YOU** goes out to all the 49 patch authors who made the 1.8.0 release possible:

Aaron Wood, Alexander Rojas, Alexander Rukletsov, Andrei Budnik, Andrei Sekretenko, Andrew Schwartzmeyer,
Armand Grillet, Benjamin Bannier, Benjamin Hindman, Benjamin Mahler, Benno Evers, Chun-Hung Hsiao,
Clement Michaud, Deepak Goel, Dominik Dary, Dragos Schebesch, Eric Chung, Fei Long, Gastón Kleiman,
Gilbert Song, Greg Mann, Ilya Pronin, Jacob Janco, James DeFelice, James Peach, Jan Schlicht,
Jason Lai, Jie Yu, Joseph Wu, Kapil Arya, Kevin Klues, Liangyu Zhao, Meng Zhu, Michael Park, Michał Łowicki,
Packt, Pavel Kirillov, Qian Zhang, Robin Gögge, Se Choi, Senthil Kumaran, Sergey Urbanovich, Tad Guo
Till Toenshoff, Tomasz Janiszewski, Vinod Kone, Xudong Ni
