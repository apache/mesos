High Availability
=========
Mesos can run in high-availability mode, in which multiple Mesos masters run simultaneously. In this mode there is only one active master, and the others masters act as stand-by replacements. These non-active masters will take over if the active master fails.

Mesos uses Apache ZooKeeper in to co-ordinate the leader election of masters.

Running mesos in this mode requires ZooKeeper. Mesos is automatically built with an included ZooKeeper client library.

Leader election and detection in Mesos is done via ZooKeeper. See: http://zookeeper.apache.org/doc/trunk/recipes.html#sc_leaderElection for more information how ZooKeeper leader election works.

Use
---------
First, a ZooKeeper cluster must be running, and a znode should be created for exclusive use by Mesos:

  - Ensure the ZooKeeper cluster is up and running.
  - Create a znode for exclusive use by mesos. The znode path will need to be provided to all slaves / scheduler drivers.

In order to spin up a Mesos cluster using multiple masters for fault-tolerance:

  - Start the mesos-master binaries using the `--zk` flag, e.g. `--zk=zk://host1:port1/path,host2:port2/path,...`
  - Start the mesos-slave binaries with `--master=zk://host1:port1/path,host2:port2/path,...`
  - Start any framework schedulers using the same zk path, the SchedulerDriver must be constructed with this path.

Refer to the Scheduler API for how to deal with leadership changes.

Semantics
---------
The detector is implemented in the `src/detector` folder. In particular, we watch for several ZooKeeper session events:

  - Connection
  - Reconnection
  - Session Expiration
  - ZNode creation, deletion, updates

We also explicitly timeout our sessions, when disconnected from ZooKeeper for an amount of time, see: `ZOOKEEPER_SESSION_TIMEOUT`. This is because the ZooKeeper client libraries only notify of session expiration upon reconnection. These timeouts are of particular interest in the case of network partitions.

Network Partitions
------------------
When a network partition occurs, if a particular component is disconnected from ZooKeeper, the Master Detector of the partitioned component will induce a timeout event. This causes the component to be notified that there is no leading master.

When slaves are master-less, they ignore incoming messages from masters to ensure that we don't act on a non-leading master's decision.

When masters enter a leader-less state, they commit suicide.

The following semantics are enforced:

  - If a slave is partitioned from the master, it will fail health-checks. The master will mark the slave as deactivated and send its tasks to LOST.
  - Deactivated slaves may not re-register with the master, and are instructed to shut down upon any further communication after deactivation.
  - When a slave is partitioned from ZooKeeper, it will enter a master-less state. It will ignore any master messages until a new master is elected.
