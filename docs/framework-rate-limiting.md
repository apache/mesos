---
title: Apache Mesos - Framework Rate Limiting
layout: documentation
---

# Framework Rate Limiting
Framework rate limiting is a feature introduced in Mesos 0.20.0.

## What is Framework Rate Limiting
In a multi-framework environment, this feature aims to protect the throughput of high-SLA (e.g., production, service) frameworks by having the master throttle messages from other (e.g., development, batch) frameworks.

To throttle messages from a framework, the Mesos cluster operator sets a `qps` (queries per seconds) value for each framework identified by its principal (You can also throttle a group of frameworks together but we'll assume individual frameworks in this doc unless otherwise stated; see the `RateLimits` [Protobuf definition](https://github.com/apache/mesos/blob/master/include/mesos/mesos.proto) and the configuration notes below). The master then promises not to process messages from that framework at a rate above `qps`. The outstanding messages are stored in memory on the master.

## Rate Limits Configuration
The following is a sample config file (in JSON format) which could be specified with the `--rate_limits` master flag.

    {
      "limits": [
        {
          "principal": "foo",
          "qps": 55.5
          "capacity": 100000
        },
        {
          "principal": "bar",
          "qps": 300
        },
        {
          "principal": "baz",
        }
      ],
      "aggregate_default_qps": 333,
      "aggregate_default_capacity": 1000000
    }

In this example, framework `foo` is throttled at the configured `qps` and `capacity`, framework `bar` is given unlimited capacity and framework `baz` is not throttled at all. If there is a fourth framework `qux` or a framework without a principal connected to the master, it is throttled by the rules `aggregate_default_qps` and `aggregate_default_capacity`.

### Configuration Notes
Below are the fields in the JSON configuration.

- **principal**: (Required) uniquely identifies the entity being throttled or given unlimited rate explicitly.
    - It should match the framework's `FrameworkInfo.principal` (See [definition](https://github.com/apache/mesos/blob/master/include/mesos/mesos.proto)).
    - You can have multiple frameworks use the same principal (e.g., some Mesos frameworks launch a new framework instance for each job), in which case the combined traffic from all frameworks using the same principal are throttled at the specified QPS.
- **qps**: (Optional) queries per second, i.e., the rate.
    - Once set, the master guarantees that it does not process messages from this principal higher than this rate. However the master could be slower than this rate, especially if the specified rate is too high.
    - To explicitly give a framework unlimited rate (i.e., not throttling it), add an entry to `limits` without the qps.
- **capacity**: (Optional) The number of *outstanding* messages frameworks of this principal can put on the master. If not specified, this principal is given unlimited capacity. Note that it is possible the queued messages use too much memory and cause the master to OOM if the capacity is set too high or not set.
    - NOTE: If `qps` is not specified, `capacity` is ignored.
- Use **aggregate_default_qps** and **aggregate_default_capacity** to safeguard the master from unspecified frameworks. All the frameworks not specified in `limits` get this default rate and capacity.
    - The rate and capacity are aggregate values for all of them, i.e., their combined traffic is throttled together.
    - Same as above, if `aggregate_default_qps` is not specified, `aggregate_default_capacity` is ignored.
    - If these fields are not present, the unspecified frameworks are not throttled.
      This is an implicit way of giving frameworks unlimited rate compared to the explicit way above (using an entry in `limits` with only the principal).
      We recommend using the explicit option especially when the master does not require authentication to prevent unexpected frameworks from overwhelming the master.

## Using Framework Rate Limiting

### Monitoring Framework Traffic
While a framework is registered with the master, the master exposes counters for all messages received and processed from that framework at its metrics endpoint: `http://<master>/metrics/snapshot`. For instance, framework `foo` has two message counters `frameworks/foo/messages_received` and `frameworks/foo/messages_processed`. Without framework rate limiting the two numbers should differ by little or none (because messages are processed ASAP) but when a framework is being throttled the difference indicates the outstanding messages as a result of the throttling.

By continuously monitoring the counters, you can derive the rate messages arrive and how fast the message queue length for the framework is growing (if it is throttled). This should depict the characteristics of the framework in terms of network traffic.

## Configuring Rate Limits
Since the goal for framework rate limiting is to prevent low-SLA frameworks from using **too much** resources and not to model their traffic and behavior as precisely as possible, you can start by using large `qps` values to throttle them. The fact that they are throttled (regardless of the configured `qps`) is already effective in giving messages from high-SLA frameworks higher priority because they are processed ASAP.

To calculate how much `capacity` the master can handle, you need to know the memory limit for the master process, the amount of memory it typically uses to serve similar workload without rate limiting (e.g., use `ps -o rss $MASTER_PID`) and average sizes of the framework messages (queued messages are stored as [serialized Protocol Buffers with a few additional fields](https://github.com/apache/mesos/blob/master/3rdparty/libprocess/include/process/message.hpp)) and you should sum up all capacity values in the config.
However since this kind of calculation is imprecise, you should start with small values that tolerate reasonable temporary framework burstiness but far from the memory limit to leave enough headroom for the master and frameworks that don't have limited capacity.

## Handling "Capacity Exceeded" Error
When a framework **exceeds the capacity**, a FrameworkErrorMessage is sent back to the framework which will [abort the scheduler driver and invoke the error() callback](https://github.com/apache/mesos/blob/master/src/sched/sched.cpp). It doesn't kill any tasks or the scheduler itself. The framework developer can choose to restart or failover the scheduler instance to remedy the consequences of dropped messages (unless your framework doesn't assume all messages sent to the master are processed).

After version 0.20.0 we are going to iterate on this feature by having the master send an early alert when the message queue for this framework **starts to build up** ([MESOS-1664](https://issues.apache.org/jira/browse/MESOS-1664), consider it a "soft limit"). The scheduler can react by throttling itself (to avoid the error message) or ignoring this alert if it's a temporary burst by design.

Before the early alerting is implemented we **don't recommend using the rate limiting feature to throttle production frameworks** for now unless you are sure about the consequences of the error message. Of course it's OK to use it to protect production frameworks by throttling other frameworks and it doesn't have any effect on the master if it's not explicitly enabled.
