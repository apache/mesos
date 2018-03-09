---
layout: post
title: Apache Mesos 0.19.0 Released
permalink: /blog/mesos-0-19-0-released/
published: true
post_author:
  display_name: Ben Mahler
  twitter: bmahler
tags: Release, Containerizer
---

The latest Mesos release, 0.19.0 is now available for [download](http://mesos.apache.org/downloads/). This new version includes the following features and improvements:

* The master now persists the list of registered slaves in a durable replicated manner using the Registrar and the replicated log.
* Alpha support for custom container technologies has been added with the [ExternalContainerizer](https://github.com/apache/mesos/blob/0.19.0/src/slave/containerizer/external_containerizer.hpp#L74).
* Metrics reporting has been overhauled and is now exposed on <ip:port>/metrics/snapshot.
* Slave Authentication: optionally, only authenticated slaves can register with the master.
* Numerous bug fixes and stability improvements.

Full release notes are available on [JIRA](https://issues.apache.org/jira/secure/ReleaseNote.jspa?projectId=12311242&version=12326253).

### Registrar

Mesos 0.19.0 introduces the ["Registrar"](https://cwiki.apache.org/confluence/display/MESOS/Registrar+Design+Document): the master now persists the list of registered slaves in a durable replicated manner. The previous lack of durable state was an intentional design decision that simplified failover and allowed masters to be run and migrated with ease. However, the stateless design had issues:

* In the event of a dual failure (slave fails while master is down), no lost task notifications are sent. This leads to a task running according to the framework but unknown to Mesos.
* When a new master is elected, we may allow rogue slaves to reregister with the master. This leads to tasks running on the slave that are not known to the framework.

Persisting the list of registered slaves allows failed over masters to detect slaves that do not reregister, and notify frameworks accordingly. It also allows us to prevent rogue slaves from reregistering; terminating the rogue tasks in the process.

The state is persisted using the [replicated log](http://mesos.apache.org/blog/mesos-0-17-0-released-featuring-autorecovery/) (available since 0.9.0).

### External Containerization

As [alluded to](http://mesos.apache.org/blog/mesos-0-18-0-released/) during the containerization / isolation refactor in 0.18.0, the ExternalContainerizer has landed in this release. This provides **alpha** level support for custom containerization.

![Mesos Containerizer Isolator APIs](/assets/img/documentation/containerizer_isolator_api.png)

Developers can implement their own external containerizers to provide support for custom container technologies. Initial Docker support is now available through some community driven external containerizers: [Docker Containerizer for Mesos](https://github.com/duedil-ltd/mesos-docker-containerizer) by Tom Arnfeld and [Deimos](https://github.com/mesosphere/deimos) by Jason Dusek. Please reach out on the mailing lists with questions!

### Metrics

Previously, Mesos components had to use custom metrics code and custom HTTP endpoints for exposing metrics. This made it difficult to expose additional system metrics and often required having an endpoint for each libprocess Process (Actor) for which metrics were desired. Having metrics spread across endpoints was operationally complex.

We needed a consistent, simple, and global way to expose metrics, which led to the creation of a metrics library within [libprocess](https://github.com/apache/mesos/tree/0.19.0/3rdparty/libprocess). All metrics are now exposed via /metrics/snapshot. The /stats.json endpoint remains for backwards compatibility.

### Upgrading

For backwards compatibility, the "Registrar" will be enabled in a phased manner. By default, the "Registrar" is write-only in 0.19.0 and will be read/write in 0.20.0.

If running in high-availability mode with ZooKeeper, operators must now specify the `--work_dir` for the master, along with the `--quorum` size of the ensemble of masters. This means adding or removing masters must be done carefully! The best practice is to only ever add or remove a single master at a time and to allow a small amount of time for the replicated log to catch up on the new master. Maintenance documentation will be added to reflect this.

Please refer to the [upgrades](http://mesos.apache.org/documentation/latest/upgrades/) document, which details how to perform an upgrade from 0.18.x.

### Future Work

Thanks to the Registrar, reconciliation primitives can now be provided to ensure that the state of tasks between Mesos and frameworks is kept consistent. This will remove the need for frameworks to implement out-of-band task reconciliation to inspect the state of slaves. Reconciliation work is being tracked at [MESOS-1407](https://issues.apache.org/jira/browse/MESOS-1407).

The addition of state through the Registrar opens up a rich set of possible features that were previously not possible due to the lack of persistent state in the master. These include:

* Cluster maintenance primitives ([MESOS-1474](https://issues.apache.org/jira/browse/MESOS-1474))
* Repair automation ([MESOS-695](https://issues.apache.org/jira/browse/MESOS-695))
* Global resource reservations

### Getting Involved

We encourage you to try out this release, and let us know what you think and if you hit any issues on the user mailing list. You can also get in touch with us via [@ApacheMesos](http://twitter.com/ApacheMesos) or via [mailing lists and IRC](http://mesos.apache.org/community/).

### Thanks

Thanks to the 32 contributors who made 0.19.0 possible:

Ashutosh Jain, Adam B, Alexandra Sava, Anton Lindström, Archana kumari, Benjamin Hindman, Benjamin Mahler, Bernardo Gomez Palacio, Bernd Mathiske, Charlie Carson, Chengwei Yang, Chi Zhang, Dave Lester, Dominic Hamon, Ian Downes, Isabel Jimenez, Jake Farrell, Jameel, Al-Aziz, Jiang Yan Xu, Jie Yu, Nikita Vetoshkin, Niklas Q. Nielsen, Ritwik Yadav, Sam Taha, Steven Phung, Till Toenshoff, Timothy St. Clair, Tobi Knaup, Tom Arnfeld, Tom Galloway, Vinod Kone, Vinson Lee