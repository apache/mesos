# Operational Guide

## Changing the master quorum
Currently the master leverages a paxos-based replicated log as its storage backend (`--registry=replicated_log` is the only storage backend supported). Each master participates in the ensemble as a log replica. The `--quorum` flag determines a majority of the masters.

The following table shows the tolerance to master failures, for each quorum size:

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
state and starting fresh. This will wipe all persistent data (e.g. slaves, maintenance
information, quota information, etc). To move from 1 master to 3 masters:

1. Stop the standalone master.
2. Remove the replicated log data (`replicated_log` under the `--work_dir`).
3. Start the original master and two new masters with `--quorum=2`

### Decreasing the quorum size

The following steps indicate how to decrement the quorum size, using 5 -> 3 masters as an example (quorum size 3 -> 2):

1. Initially, 5 masters are running with `--quorum=3`
2. Remove 2 masters from the cluster, ensure they will not be restarted (See NOTE section above). Now 3 masters are running with `--quorum=3`
3. Restart the 3 masters with `--quorum=2`

To decrease the quorum by N, repeat this process to decrement the quorum size N times.

### Replacing a master
Please see the NOTE section above. So long as the failed master is guaranteed to not re-join the ensemble, it is safe to start a new master _with an empty log_ and allow it to catch up.

## External access for mesos master
If the default ip (or the command line arg `--ip`) points to an internal IP, then external entities such as framework scheduler would not be able to reach the master. To address that scenario, an externally accessible IP:port can be setup via the `--advertise_ip` and `--advertise_port` command line arguments of mesos master. If configured, external entities such as framework scheduler interact with the advertise_ip:advertise_port from where the request needs to be proxied to the internal IP:Port on which mesos master is listening.
