---
layout: post
title: Slave Recovery in Apache Mesos
published: true
post_author:
  display_name: Vinod Kone
  gravatar: 24bc66008e50fb936e696735e99d7815
  twitter: vinodkone
tags: HighAvailability, SlaveRecovery
---

High availability is one of the key features of Mesos. For example, a typical Mesos cluster in production involves 3-5 masters with one acting as *leader* and the rest on standby. When a leading master fails due to a crash or goes offline for an upgrade, a standby master automatically becomes the leader without causing any disruption to running services. Leader election is currently performed by using [ZooKeeper](http://zookeeper.apache.org/).

With the latest Mesos release, [0.14.1](http://mesos.apache.org/downloads/), we are bringing high availability to the slaves by introducing a new feature called *Slave Recovery*. In a nutshell, slave recovery enables:

1. Executors/tasks to keep running when the slave process is down.

2. A restarted slave process to reconnect with running executors/tasks on the slave.

## Why it matters

A recoverable slave is critical for running services in production on Mesos for several reasons:

- **Stateful services**

    In a typical production environment there are stateful services (e.g., caches) running in the cluster. It is not uncommon for these services to have a high startup time (e.g., cache warm up time of a few hours). Even in the analytics world, there are cases where a single task is responsible for doing work that takes hours to complete. In such cases a restart of the slave (e.g, crash or upgrade) will have a huge impact on the service. While sharding the service wider mitigates this impact it is not always possible to do so (e.g, legacy services, data locality, application semantics). Such stateful applications would benefit immensely from running under a more resilient slave.
    
- **Faster cluster upgrades**

    It is important for clusters to frequently upgrade their infrastructure to stay up-to-date with the latest features and bug fixes. A typical Mesos slave upgrade involves stopping the slave, upgrading the slave libraries and starting the slave. In production environments there is always tension between upgrading the infrastructure frequently and the need to not impact long running services as much as possible. With respect to Mesos upgrades, if upgrading the slave binary has no impact on the underlying services, then it is a win for both the cluster operators and the service owners.

- **Resilience against slave failures**

    While upgrading the slaves is most often the reason for restarting slaves, there might be other causes for a slave to fail. A slave crash could happen due to a bug in the slave code or due to external factors like a bug in the kernel or ZooKeeper client library. Typically such crashes are temporary and a restart of the slave is enough to correct the situation. If such slave restarts do not affect applications running on the slave it is a huge win for the applications.

## How it works

- **Checkpointing**
     
    Slave recovery works by having the slave checkpoint enough information (e.g., task information, executor information, status updates) about the running tasks and executors to local disk. Once the slave and the framework(s) enable checkpointing, any subsequent slave restarts would recover the checkpointed information and reconnect with the executors. When a checkpointing slave process goes down, both the leading master and the executors running on the slave host wait for the slave to come back up and reconnect. A nice thing about slave recovery is that frameworks and their executors/tasks are oblivious to a slave restart.

- **Executor Driver Caching**

    As part of this feature, the executor driver has also been improved to make it more resilient in the face of a slave failure. As an example, status updates sent by the executor while the slave is down are cached by the driver and sent to the slave when it reconnects with the restarted slave. Since this is all handled by the executor driver, framework/executor writers do not have to worry about it! The executors can keep sending status updates for their tasks while remaining oblivious to the slave being up or down.

- **Reliable status updates**

    Another benefit of slave checkpointing the status updates is that now updates are more reliably delivered to frameworks in the face of failures. Before slave recovery if the slave fails at the same time that a master is failing over, no TASK_LOST updates for tasks running on the slave were sent to the framework. This is partly because the Mesos master is stateless. A failed over master reconstructs the cluster state from the information given to it by reregistering slaves and frameworks. With slave recovery, status updates and tasks are no longer lost when slaves fail. Rather, the slave recovers tasks, status updates and reconnects with the running executors. Even if an executor does terminate when the slave is down, a recovered slave knows about it from its checkpointed state and reliably sends TASK_LOST updates to the framework.

For more information about how to enable slave recovery in your cluster, please refer to the [documentation](https://github.com/apache/mesos/blob/master/docs/Slave-Recovery.md).

## Looking ahead

- **Easy Executor/Task Upgrades**

    In a similar vein to how slave recovery makes upgrading a Mesos cluster easy, we would like to enable frameworks to upgrade their executors/tasks as well. Currently the only way to upgrade an executor/task is to kill the old executor/task and launch the new upgraded executor/task. For the same reasons as we have discussed earlier this is not ideal for stateful services. We are currently investigating  proper primitives to provide to frameworks to do such upgrades, so that not every framework have to (re-)implement that logic.

- **State Reconciliation**

    While slave recovery greatly improves the reliability of delivering status updates there are still some rare cases where updates could be lost. For example if a slave crashes when a master is failing over and never comes back then the new leading master doesn't know about the lost slave and executors/tasks running on it. In addition to status updates, any driver methods (e.g., launchTasks, killTask) invoked when the master is failing over are silently dropped. Currently, frameworks are responsible for reconciling their own task state using the [reconciliation API](https://github.com/apache/mesos/blob/master/include/mesos/scheduler.hpp#L290). We are currently investigating ways to provide even better guarantees  around reconciliation.

- **Self Updates of Mesos**

    Currently, updating Mesos involves a cluster operator to manually upgrade the master and slave binaries and roll them in a specific manner (e.g., masters before slaves). But what if Mesos can update itself! It is not hard to imagine a future where Mesos masters can orchestrate the upgrade of slaves and maybe also upgrade one another! This would also help making rollbacks easy incase an upgrade doesn’t work because it would be much easier Mesos to check if various components are working as expected.
  
So what are you waiting for? Go ahead and give [Mesos](http://mesos.apache.org) a whirl and [let us know](mailto:user@mesos.apache.org) what you think.