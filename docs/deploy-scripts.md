---
title: Apache Mesos - Deployment Scripts
layout: documentation
---

# Deployment Scripts

Mesos includes a set of scripts in `[install-prefix]/sbin` that can be used to deploy it on a cluster. To use these scripts, you need to create two configuration files:

1. `[install-prefix]/var/mesos/deploy/masters`, which should list the hostname(s) of the node(s) you want to be your masters (one per line), and
2. `[install-prefix]/var/mesos/deploy/agents`, which should contain a list of hostnames for your agents (one per line).

You can then start a cluster with `[install-prefix]/sbin/mesos-start-cluster.sh` and stop it with `[install-prefix]/sbin/mesos-stop-cluster.sh`.

It is also possible to set environment variables, ulimits, etc that will affect the master and agent by editing `[install-prefix]/var/mesos/deploy/mesos-deploy-env.sh`. One particularly useful setting is `LIBPROCESS_IP`, which tells the master and agent binaries which IP address to bind to; in some installations, the default interface that the hostname resolves to is not the machine's external IP address, so you can set the right IP through this variable. Besides the common environment variables of master and agent configured in `[install-prefix/var/mesos/deploy/mesos-deploy-env.sh`, it is also possible to set master specific environment variables in `[install-prefix]/var/mesos/deploy/mesos-master-env.sh`, agent specific environment variables in `[install-prefix]/var/mesos/deploy/mesos-agent-env.sh`.

Finally, the deploy scripts do not use ZooKeeper by default. If you want to configure Mesos to use ZooKeeper to coordinate multiple master nodes, please see the [High Availability](high-availability.md) documentation for details.

## Notes

* The deploy scripts assume that Mesos is located in the same directory on all nodes.
* If you want to enable multiple Unix users to submit to the same cluster, you need to run the Mesos agents as root (or possibly set the right attributes on the `mesos-agent` binary). Otherwise, they will fail to `setuid`.
