---
title: Apache Mesos - Operational Guide
layout: documentation
---

# Operational Guide

## Using a process supervisor
Mesos uses a "[fail-fast](https://en.wikipedia.org/wiki/Fail-fast)" approach to error handling: if a serious error occurs, Mesos will typically exit rather than trying to continue running in a possibly erroneous state. For example, when Mesos is configured for [high availability](high-availability.md), the leading master will abort itself when it discovers it has been partitioned away from the Zookeeper quorum. This is a safety precaution to ensure the previous leader doesn't continue communicating in an unsafe state.

To ensure that such failures are handled appropriately, production deployments of Mesos typically use a _process supervisor_ (such as systemd or supervisord) to detect when Mesos processes exit. The supervisor can be configured to restart the failed process automatically and/or to notify the cluster operator to investigate the situation.

## Changing the master quorum
The master leverages a [Paxos-based replicated log](replicated-log-internals.md) as its storage backend (`--registry=replicated_log` is the only storage backend currently supported). Each master participates in the ensemble as a log replica. The `--quorum` flag determines a majority of the masters.

The following table shows the tolerance to master failures for each quorum size:

| Masters  | Quorum Size | Failure Tolerance |
| -------: | ----------: | ----------------: |
|        1 |           1 |                 0 |
|        3 |           2 |                 1 |
|        5 |           3 |                 2 |
|      ... |         ... |               ... |
|   2N - 1 |           N |             N - 1 |

It is recommended to run with 3 or 5 masters, when desiring high availability.

### NOTE
When configuring the quorum, it is essential to ensure that there are only so many masters running as specified in the table above. If additional masters are running, this violates the quorum and the log may be corrupted! As a result, it is recommended to gate the running of the master process with something that enforces a static whitelist of the master hosts. See [MESOS-1546](https://issues.apache.org/jira/browse/MESOS-1546) for adding a safety whitelist within Mesos itself.

For online reconfiguration of the log, see: [MESOS-683](https://issues.apache.org/jira/browse/MESOS-683).

### Increasing the quorum size
As the size of a cluster grows, it may be desired to increase the quorum size for additional fault tolerance.

The following steps indicate how to increment the quorum size, using 3 -> 5 masters as an example (quorum size 2 -> 3):

1. Initially, 3 masters are running with `--quorum=2`
2. Restart the original 3 masters with `--quorum=3`
3. Start 2 additional masters with `--quorum=3`

To increase the quorum by N, repeat this process to increment the quorum size N times.

NOTE: Currently, moving out of a single master setup requires wiping the replicated log
state and starting fresh. This will wipe all persistent data (e.g., agents, maintenance
information, quota information, etc). To move from 1 master to 3 masters:

1. Stop the standalone master.
2. Remove the replicated log data (`replicated_log` under the `--work_dir`).
3. Start the original master and two new masters with `--quorum=2`

### Decreasing the quorum size

The following steps indicate how to decrement the quorum size, using 5 -> 3 masters as an example (quorum size 3 -> 2):

1. Initially, 5 masters are running with `--quorum=3`
2. Remove 2 masters from the cluster, ensure they will not be restarted (see NOTE section above). Now 3 masters are running with `--quorum=3`
3. Restart the 3 masters with `--quorum=2`

To decrease the quorum by N, repeat this process to decrement the quorum size N times.

### Replacing a master
Please see the NOTE section above. So long as the failed master is guaranteed to not re-join the ensemble, it is safe to start a new master _with an empty log_ and allow it to catch up.

## External access for Mesos master
If the default IP (or the command line arg `--ip`) is an internal IP, then external entities such as framework schedulers will be unable to reach the master. To address that scenario, an externally accessible IP:port can be setup via the `--advertise_ip` and `--advertise_port` command line arguments of `mesos-master`. If configured, external entities such as framework schedulers interact with the advertise_ip:advertise_port from where the request needs to be proxied to the internal IP:port on which the Mesos master is listening.

## HTTP requests to non-leading master
HTTP requests to some master endpoints (e.g., [/state](endpoints/master/state.md), [/machine/down](endpoints/master/machine/down.md)) can only be answered by the leading master. Such requests made to a non-leading master will result in either a `307 Temporary Redirect` (with the location of the leading master) or `503 Service Unavailable` (if the master does not know who the current leader is).
