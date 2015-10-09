---
layout: post
title: Mesos 0.17.0 Released, Featuring Autorecovery for the Replicated Log
permalink: /blog/mesos-0-17-0-released-featuring-autorecovery/
published: true
post_author:
  display_name: Jie Yu
  twitter: jie_yu
tags: Release, Autorecovery, Replicatedlog
---

The latest Mesos release, [0.17.0](http://mesos.apache.org/downloads/) was made available for [download](http://mesos.apache.org/downloads) last week, introducing a new autorecovery feature for the replicated log that handles additional disk failure and operator error scenarios.

## What is the replicated log?

The replicated log is a distributed _append-only_ log that stores arbitrary data as entries.  Replication comes into play as at least a quorum of log replicas are stored on different machines, protecting the data from individual disk or network failures. The replicated log uses the [Paxos algorithm](http://en.wikipedia.org/wiki/Paxos_(computer_science%29). We will describe additional implementation details in both an upcoming blog post and project documentation.

Mesos has had replicated log since version 0.9.0 and [Apache Aurora](http://aurora.incubator.apache.org/), a framework used in production at Twitter, uses replicated log for its persistent storage.  Additionally, the Mesos master will start using the replicated log to persist some cluster state with the introduction of the [registrar](https://issues.apache.org/jira/browse/MESOS-764).

## New feature in Mesos 0.17.0: _autorecovery_

Before Mesos version 0.17.0, the replicated log could become inconsistent if a replica’s disk was replaced or the data was accidentally deleted by an operator. That meant that an operator couldn’t safely, or easily, replace a replica’s disk while the other replicas kept running. Instead, an operator had to stop all replicas, replace the disk, copy a pre-existing log, then restart all the replicas.

As of this release, a replica with a new disk can automatically recover without operator involvement. As long as a quorum of replicas are available, applications using the replicated log won’t notice a thing!

## Future work

The new autorecovery feature enables replacing a single replica at a time, but it does not allow an operator to easily add or remove replicas, known as _reconfiguration_. You can follow [MESOS-683](https://issues.apache.org/jira/browse/MESOS-683) for the latest on this future work.