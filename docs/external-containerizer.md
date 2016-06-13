---
title: Apache Mesos - External Containerizer
layout: documentation
---

# External Containerizer

**NOTE:**  The external containerizer is deprecated. See
[MESOS-3370](https://issues.apache.org/jira/browse/MESOS-3370) for details.

* EC = external containerizer. A part of the mesos agent that provides
an API for containerizing via external plugin executables.
* ECP = external containerizer program. An external plugin executable
implementing the actual containerizing by interfacing with a
containerizing system (e.g. Docker).

# Containerizing


# General Overview

EC invokes ECP as a shell process, passing the command as a parameter
to the ECP executable. Additional data is exhanged via stdin and
stdout.

The ECP is expected to return a zero exit code for all commands it was
able to process. A non-zero status code signals an error. Below you
will find an overview of the commands that have to be implemented by
an ECP, as well as their invocation scheme.

The ECP is expected to be using stderr for state info and displaying
additional debug information. That information is getting logged to
a file, see [Enviroment: **Sandbox**](#sandbox).


### Call and communication scheme

Interface describing the functions an ECP has to implement via
command calls. Many invocations on the ECP will also pass a
protobuf message along via stdin. Some invocations on the ECP also
expect to deliver a result protobuf message back via stdout.
All protobuf messages are prefixed by their original length -
this is sometimes referred to as "Record-IO"-format. See
[Record-IO De/Serializing Example](#record-io-deserializing-example).

**COMMAND < INPUT-PROTO > RESULT-PROTO**

* `launch < containerizer::Launch`
* `update < containerizer::Update`
* `usage < containerizer::Usage > mesos::ResourceStatistics`
* `wait < containerizer::Wait > containerizer::Termination`
* `destroy < containerizer::Destroy`
* `containers > containerizer::Containers`
* `recover`


# Command Ordering

## Make no assumptions
Commands may pretty much come in any order. There is only one
exception to this rule; when launching a task, the EC will make sure
that the ECP first receives a `launch` on that specific container, all
other commands are queued until `launch` returns from the ECP.


# Use Cases

## Task Launching EC / ECP Overview

* EC invokes `launch` on the ECP.
 * Along with that call, the ECP will receive a containerizer::Launch
 protobuf message via stdin.
 * ECP now makes sure the executor gets started.
**Note** that `launch` is not supposed to block. It should return
immediately after triggering the executor/command - that could be done
via fork-exec within the ECP.
* EC invokes `wait` on the ECP.
 * Along with that call, the ECP will receive a containerizer::Wait
 protobuf message via stdin.
 * ECP now blocks until the launched command is reaped - that could be
 implemented via waitpid within the ECP.
 * Once the command is reaped, the ECP should deliver a
 containerizer::Termination protobuf message via stdout, back to the
 EC.


## Container Lifecycle Sequence Diagrams


### Container Launching

A container is in a staging state and now gets started and observed
until it gets into a final state.

![Container Launching Scheme](images/ec_launch_seqdiag.png?raw=true)

### Container Running

A container has gotten launched at some point and now is considered
being in a non terminal state by the agent. The following commands
will get triggered multiple times at the ECP over the lifetime of a
container. Their order however is not determined.

![Container Running Scheme](images/ec_lifecycle_seqdiag.png?raw=true)

### Resource Limitation

While a container is active, a resource limitation was identified
(e.g. out of memory) by the ECP isolation mechanism of choice.

![Resource Limitation Scheme](images/ec_kill_seqdiag.png?raw=true)

<a name="agent-recovery-overview"></a>
## Agent Recovery Overview

* Agent recovers via check pointed state.
* EC invokes `recover` on the ECP - there is no protobuf message sent
or expected as a result from this command.
 * The ECP may try to recover internal states via its own failover
mechanisms, if needed.
* After `recover` returns, the EC will invoke `containers` on the ECP.
 * The ECP should return Containers which is a list of currently
 active containers.
**Note** these containers are known to the ECP but might in fact
partially be unknown to the agent (e.g. agent failed after launch but
before or within wait) - those containers are considered to be
orphans.
* The EC now compares the list of agent known containers to those
listed within `Containers`. For each orphan it identifies, the agent
will invoke a `wait` followed by a `destroy` on the ECP for those
containers.
* Agent will now call `wait` on the ECP (via EC) for all recovered
containers. This does once again put `wait` into the position of the
ultimate command reaper.


## Agent Recovery Sequence Diagram

### Recovery

While containers are active, the agent fails over.

![Recovery Scheme](images/ec_recover_seqdiag.png?raw=true)

### Orphan Destruction

Containers identified by the ECP as being active but not agent state
recoverable are getting terminated.

![Orphan Destruction Scheme](images/ec_orphan_seqdiag.png?raw=true)


# Command Details

## launch
### Start the containerized executor

Hands over all information the ECP needs for launching a task
via an executor.
This call should not wait for the executor/command to return. The
actual reaping of the containerized command is done via the `wait`
call.

    launch < containerizer::Launch

This call receives the containerizer::Launch protobuf via stdin.

    /**
     * Encodes the launch command sent to the external containerizer
     * program.
     */
    message Launch {
      required ContainerID container_id = 1;
      optional TaskInfo task_info = 2;
      optional ExecutorInfo executor_info = 3;
      optional string directory = 4;
      optional string user = 5;
      optional SlaveID agent_id = 6;
      optional string agent_pid = 7;
      optional bool checkpoint = 8;
    }

This call does not return any data via stdout.

## wait
### Gets information on the containerized executor's Termination

Is expected to reap the executor/command. This call should block
until the executor/command has terminated.

    wait < containerizer::Wait > containerizer::Termination

This call receives the containerizer::Wait protobuf via stdin.

    /**
     * Encodes the wait command sent to the external containerizer
     * program.
     */
    message Wait {
      required ContainerID container_id = 1;
    }

This call is expected to return containerizer::Termination via stdout.

    /**
     * Information about a container termination, returned by the
     * containerizer to the agent.
     */
    message Termination {
      // A container may be killed if it exceeds its resources; this will
      // be indicated by killed=true and described by the message string.
      required bool killed = 1;
      required string message = 2;

      // Exit status of the process.
      optional int32 status = 3;
    }

The Termination attribute `killed` is to be set only when the
containerizer or the underlying isolation had to enforce a limitation
by killing the task (e.g. task exceeded suggested memory limit).

## update
### Updates the container's resource limits

Is sending (new) resource constraints for the given container.
Resource constraints onto a container may vary over the lifetime of
the containerized task.

    update < containerizer::Update

This call receives the containerizer::Update protobuf via stdin.

    /**
     * Encodes the update command sent to the external containerizer
     * program.
     */
    message Update {
      required ContainerID container_id = 1;
      repeated Resource resources = 2;
    }

This call does not return any data via stdout.

## usage
### Gathers resource usage statistics for a containerized task
Is used for polling the current resource uses for the given container.

    usage < containerizer::Usage > mesos::ResourceStatistics

This call received the containerizer::Usage protobuf via stdin.

    /**
     * Encodes the usage command sent to the external containerizer
     * program.
     */
    message Usage {
      required ContainerID container_id = 1;
    }

This call is expected to return mesos::ResourceStatistics via stdout.

    /*
     * A snapshot of resource usage statistics.
     */
    message ResourceStatistics {
      required double timestamp = 1; // Snapshot time, in seconds since the Epoch.

      // CPU Usage Information:
      // Total CPU time spent in user mode, and kernel mode.
      optional double cpus_user_time_secs = 2;
      optional double cpus_system_time_secs = 3;

      // Number of CPUs allocated.
      optional double cpus_limit = 4;

      // cpu.stat on process throttling (for contention issues).
      optional uint32 cpus_nr_periods = 7;
      optional uint32 cpus_nr_throttled = 8;
      optional double cpus_throttled_time_secs = 9;

      // Memory Usage Information:
      optional uint64 mem_rss_bytes = 5; // Resident Set Size.

      // Amount of memory resources allocated.
      optional uint64 mem_limit_bytes = 6;

      // Broken out memory usage information (files, anonymous, and mmaped files)
      optional uint64 mem_file_bytes = 10;
      optional uint64 mem_anon_bytes = 11;
      optional uint64 mem_mapped_file_bytes = 12;
    }

## destroy
### Terminates the containerized executor

Is used in rare situations, like for graceful agent shutdown
but also in agent fail over scenarios - see Agent Recovery for more.

    destroy < containerizer::Destroy

This call receives the containerizer::Destroy protobuf via stdin.

    /**
     * Encodes the destroy command sent to the external containerizer
     * program.
     */
    message Destroy {
      required ContainerID container_id = 1;
    }

This call does not return any data via stdout.

## containers
### Gets all active container-id's

Returns all container identifiers known to be currently active.

    containers > containerizer::Containers

This call does not receive any additional data via stdin.

This call is expected to pass containerizer::Containers back via
stdout.

    /**
     * Information on all active containers returned by the containerizer
     * to the agent.
     */
    message Containers {
      repeated ContainerID containers = 1;
    }


## recover
### Internal ECP state recovery

Allows the ECP to do a state recovery on its own. If the ECP
uses state check-pointing e.g. via file system, then this call would
be a good moment to de-serialize that state information. Make sure you
also see [Agent Recovery Overview](#agent-recovery-overview) for more.

    recover

This call does not receive any additional data via stdin.
No returned data via stdout.



### Protobuf Message Definitions

For possibly more up-to-date versions of the above mentioned protobufs
as well as protobuf messages referenced by them, please check:

* containerizer::XXX are defined within
  include/mesos/containerizer/containerizer.proto.

* mesos::XXX are defined within include/mesos/mesos.proto.



# Environment

<a name="sandbox"></a>
## **Sandbox**

A sandbox environment is formed by `cd` into the work-directory of the
executor as well as a stderr redirect into the executor's "stderr"
log-file.
**Note** not **all** invocations have a complete sandbox environment.


## Addional Environment Variables

Additionally, there are a few new environment variables set when
invoking the ECP.


* MESOS_LIBEXEC_DIRECTORY = path to mesos-executor, mesos-usage, ...
This information is always present.

* MESOS_WORK_DIRECTORY = agent work directory. This should be used for
distinguishing agent instances.
This information is always present.

**Note** that this is specifically helpful for being able to tie a set
of containers to a specific agent instance, thus allowing proper
recovery when needed.

* MESOS_DEFAULT_CONTAINER_IMAGE = default image as provided via agent
flags (default_container_image). This variable is provided only in
calls to `launch`.



# Debugging

<a name="enhanced-verbosity-logging"></a>
## Enhanced Verbosity Logging

For receiving an increased level of status information from the EC
use the GLOG verbosity level. Prefix your mesos startup call by
setting the level to a value higher than or equal to two.

`GLOG_v=2 ./bin/mesos-agent --master=[...]`


## ECP stderr Logging

All output to stderr of your ECP will get logged to the executor's
'stderr' log file.
The specific location can be extracted from the [Enhanced Verbosity
Logging](#enhanced-verbosity-logging) of the EC.

Example Log Output:

    I0603 02:12:34.165662 174215168 external_containerizer.cpp:1083] Invoking external containerizer for method 'launch'
    I0603 02:12:34.165675 174215168 external_containerizer.cpp:1100] calling: [/Users/till/Development/mesos-till/build/src/test-containerizer launch]
    I0603 02:12:34.165678 175824896 agent.cpp:497] Successfully attached file '/tmp/ExternalContainerizerTest_Launch_lP22ci/agents/20140603-021232-16777343-51377-7591-0/frameworks/20140603-021232-16777343-51377-7591-0000/executors/1/runs/558e0a69-70da-4d71-b4c4-c2820b1d6345'
    I0603 02:12:34.165686 174215168 external_containerizer.cpp:1101] directory: /tmp/ExternalContainerizerTest_Launch_lP22ci/agents/20140603-021232-16777343-51377-7591-0/frameworks/20140603-021232-16777343-51377-7591-0000/executors/1/runs/558e0a69-70da-4d71-b4c4-c2820b1d6345

The stderr output of the ECP for this call is found within the stderr file located in the directory displayed in the last quoted line.

    cat /tmp/ExternalContainerizerTest_Launch_lP22ci/agents/20140603-021232-16777343-51377-7591-0/frameworks/20140603-021232-16777343-51377-7591-0000/executors/1/runs/558e0a69-70da-4d71-b4c4-c2820b1d6345/stderr


# Appendix

## Record-IO Proto Example: Launch

This is what a properly record-io formatted protobuf looks like.

**name:    offset**

* length: 00 - 03 = record length in byte

* payload: 04 - (length + 4) = protobuf payload

Example length: 00000240h = 576 byte total protobuf size

Example Hexdump:

    00000000:  4002 0000 0a26 0a24 3433 3532 3533 6162 2d64 3234 362d 3437  :@....&.$435253ab-d246-47
    00000018:  6265 2d61 3335 302d 3335 3432 3034 3635 6438 3638 1a81 020a  :be-a350-35420465d868....
    00000030:  030a 0131 2a16 0a04 6370 7573 1000 1a09 0900 0000 0000 0000  :...1*...cpus............
    00000048:  4032 012a 2a15 0a03 6d65 6d10 001a 0909 0000 0000 0000 9040  :@2.**...mem............@
    00000060:  3201 2a2a 160a 0464 6973 6b10 001a 0909 0000 0000 0000 9040  :2.**...disk............@
    00000078:  3201 2a2a 180a 0570 6f72 7473 1001 220a 0a08 0898 f201 1080  :2.**...ports..".........
    00000090:  fa01 3201 2a3a 2a1a 2865 6368 6f20 274e 6f20 7375 6368 2066  :..2.*:*.(echo 'No such f
    000000a8:  696c 6520 6f72 2064 6972 6563 746f 7279 273b 2065 7869 7420  :ile or directory'; exit
    000000c0:  3142 2b0a 2932 3031 3430 3532 362d 3031 3530 3036 2d31 3637  :1B+.)20140526-015006-167
    000000d8:  3737 3334 332d 3535 3430 332d 3632 3536 372d 3030 3030 4a3d  :77343-55403-62567-0000J=
    000000f0:  436f 6d6d 616e 6420 4578 6563 7574 6f72 2028 5461 736b 3a20  :Command Executor (Task:
    00000108:  3129 2028 436f 6d6d 616e 643a 2073 6820 2d63 2027 7768 696c  :1) (Command: sh -c 'whil
    00000120:  6520 7472 7565 203b 2e2e 2e27 2952 0131 22c5 012f 746d 702f  :e true ;...')R.1"../tmp/
    00000138:  4578 7465 726e 616c 436f 6e74 6169 6e65 7269 7a65 7254 6573  :ExternalContainerizerTes
    00000150:  745f 4c61 756e 6368 5f6c 5855 6839 662f 736c 6176 6573 2f32  :t_Launch_lXUh9f/agents/2
    00000168:  3031 3430 3532 362d 3031 3530 3036 2d31 3637 3737 3334 332d  :0140526-015006-16777343-
    00000180:  3535 3430 332d 3632 3536 372d 302f 6672 616d 6577 6f72 6b73  :55403-62567-0/frameworks
    00000198:  2f32 3031 3430 3532 362d 3031 3530 3036 2d31 3637 3737 3334  :/20140526-015006-1677734
    000001b0:  332d 3535 3430 332d 3632 3536 372d 3030 3030 2f65 7865 6375  :3-55403-62567-0000/execu
    000001c8:  746f 7273 2f31 2f72 756e 732f 3433 3532 3533 6162 2d64 3234  :tors/1/runs/435253ab-d24
    000001e0:  362d 3437 6265 2d61 3335 302d 3335 3432 3034 3635 6438 3638  :6-47be-a350-35420465d868
    000001f8:  2a04 7469 6c6c 3228 0a26 3230 3134 3035 3236 2d30 3135 3030  :*.till2(.&20140526-01500
    00000210:  362d 3136 3737 3733 3433 2d35 3534 3033 2d36 3235 3637 2d30  :6-16777343-55403-62567-0
    00000228:  3a18 736c 6176 6528 3129 4031 3237 2e30 2e30 2e31 3a35 3534  ::.slave(1)@127.0.0.1:554
    00000240:  3033 4000

<a name="record-io-deserializing-example"></a>
## Record-IO De/Serializing Example
How to send and receive such record-io formatted message
using Python

*taken from src/examples/python/test_containerizer.py*

    # Read a data chunk prefixed by its total size from stdin.
    def receive():
        # Read size (uint32 => 4 bytes).
        size = struct.unpack('I', sys.stdin.read(4))
        if size[0] <= 0:
            print >> sys.stderr, "Expected protobuf size over stdin. " \
                             "Received 0 bytes."
            return ""

        # Read payload.
        data = sys.stdin.read(size[0])
        if len(data) != size[0]:
            print >> sys.stderr, "Expected %d bytes protobuf over stdin. " \
                             "Received %d bytes." % (size[0], len(data))
            return ""

        return data

    # Write a protobuf message prefixed by its total size (aka recordio)
    # to stdout.
    def send(data):
        # Write size (uint32 => 4 bytes).
        sys.stdout.write(struct.pack('I', len(data)))

        # Write payload.
        sys.stdout.write(data)
