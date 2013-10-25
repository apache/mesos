# Slave Recovery #

Slave recovery is a feature of Mesos that allows:

 1. Executors/tasks to keep running when the slave process is down and
 2. Allows a restarted slave process to reconnect with running executors/tasks on the slave.

Mesos slave could be restarted for an upgrade or due to a crash. This feature is introduced in ***0.14.0*** release.

### How does it work? ###

Slave recovery works by having the slave checkpoint enough information (e.g., Task Info, Executor Info, Status Updates) about the running tasks and executors to local disk. Once the slave ***and*** the framework(s) enable checkpointing, any subsequent slave restarts would recover
the checkpointed information and reconnect with the executors. Note that if the host running the slave process is rebooted all the executors/tasks are killed.

> NOTE: To enable slave recovery both the slave and the framework should explicitly request checkpointing.
> Alternatively, a framework that doesn't want the disk i/o overhead of checkpointing can opt out of checkpointing.


### Enabling slave checkpointing ###

As part of this feature, 4 new flags were added to the slave.

  - `checkpoint` :  Whether to checkpoint slave and frameworks information
                    to disk [Default: false].
    - This enables a restarted slave to recover status updates and reconnect
      with (--recover=reconnect) or kill (--recover=kill) old executors.

  - `strict` : Whether to do recovery in strict mode [Default: true].
    - If strict=true, any and all recovery errors are considered fatal.
    - If strict=false, any errors (e.g., corruption in checkpointed data) during recovery are
      ignored and as much state as possible is recovered.

  - `recover` : Whether to recover status updates and reconnect with old executors [Default: reconnect].
     - If recover=reconnect, Reconnect with any old live executors.
     - If recover=cleanup, Kill any old live executors and exit.
       Use this option when doing an incompatible slave or executor upgrade!).
       NOTE: If no checkpointing information exists, no recovery is performed
       and the slave registers with the master as a new slave.

  - `recovery_timeout` : Amount of time allotted for the slave to recover [Default: 15 mins].
     - If the slave takes longer than `recovery_timeout` to recover, any executors that are waiting to
     reconnect to the slave will self-terminate.
     NOTE: This flag is only applicable when `--checkpoint` is enabled.


> NOTE: If checkpointing is enabled on the slave, but none of the frameworks have enabled checkpointing,
> executors/tasks of frameworks die when the slave dies and are not recovered.

A restarted slave should re-register with master within a timeout (currently, 75s). If the slave takes longer
than this timeout to re-register, the master shuts down the slave, which in turn shuts down any live executors/tasks.
Therefore, it is highly recommended to automate the process of restarting a slave (e.g, using [monit](http://mmonit.com/monit/)).


**For the complete list of slave options: ./mesos-slave.sh --help**


### Enabling framework checkpointing ###

As part of this feature, `FrameworkInfo` has been updated to include an optional `checkpoint` field. A framework that would like to opt in to checkpointing should set `FrameworkInfo.checkpoint=True` before registering with the master.

> NOTE: Frameworks that have enabled checkpointing will only get offers from checkpointing slaves. So, before setting `checkpoint=True` on FrameworkInfo, ensure that there are slaves in your cluster that have enabled checkpointing.
> Because, if there are no checkpointing slaves, the framework would not get any offers and hence cannot launch any tasks/executors!


### Upgrading to 0.14.0 ###

If you want to upgrade a running Mesos cluster to 0.14.0 to take advantage of slave recovery please follow the [upgrade instructions](https://github.com/apache/mesos/blob/master/docs/Upgrades.md).
