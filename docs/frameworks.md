---
layout: documentation
---
# Software projects built on Mesos

## Long Running Services

* [Aurora](http://aurora.incubator.apache.org) is a service scheduler that runs on top of Mesos, enabling you to run long-running services that take advantage of Mesos' scalability, fault-tolerance, and resource isolation.
* [Marathon](https://github.com/mesosphere/marathon) is a private PaaS built on Mesos. It automatically handles hardware or software failures and ensures that an app is "always on".
* [Singularity](https://github.com/HubSpot/Singularity) is a scheduler (HTTP API and web interface) for running Mesos tasks: long running processes, one-off tasks, and scheduled jobs.
* [SSSP](https://github.com/mesosphere/sssp) is a simple web application that provides a white-label "Megaupload" for storing and sharing files in S3.

## Big Data Processing

* [Cray Chapel](https://github.com/nqn/mesos-chapel) is a productive parallel programming language. The Chapel Mesos scheduler lets you run Chapel programs on Mesos.
* [Dpark](https://github.com/douban/dpark) is a Python clone of Spark, a MapReduce-like framework written in Python, running on Mesos.
* [Exelixi](https://github.com/mesosphere/exelixi) is a distributed framework for running genetic algorithms at scale.
* [Hadoop](https://github.com/mesos/hadoop) Running Hadoop on Mesos distributes MapReduce jobs efficiently across an entire cluster.
* [Hama](http://wiki.apache.org/hama/GettingStartedMesos) is a distributed computing framework based on Bulk Synchronous Parallel computing techniques for massive scientific computations e.g., matrix, graph and network algorithms.
* [MPI](https://github.com/mesosphere/mesos-hydra) is a message-passing system designed to function on a wide variety of parallel computers.
* [Spark](http://spark.incubator.apache.org/) is a fast and general-purpose cluster computing system which makes parallel jobs easy to write.
* [Storm](https://github.com/mesosphere/storm-mesos) is a distributed realtime computation system. Storm makes it easy to reliably process unbounded streams of data, doing for realtime processing what Hadoop did for batch processing.

## Batch Scheduling

* [Chronos](https://github.com/airbnb/chronos) is a distributed job scheduler that supports complex job topologies. It can be used as a more fault-tolerant replacement for Cron.
* [Jenkins](https://github.com/jenkinsci/mesos-plugin) is a continuous integration server. The mesos-jenkins plugin allows it to dynamically launch workers on a Mesos cluster depending on the workload.
* [JobServer](http://www.grandlogic.com/content/html_docs/jobserver.html) is a distributed job scheduler and processor  which allows developers to build custom batch processing Tasklets using point and click web UI.
* [GoDocker](https://bitbucket.org/osallou/go-docker) is a batch computing job scheduler like SGE, Torque, etc. It schedules batch computing tasks via webui, API or CLI for system or LDAP users, mounting their home directory or other shared resources in a Docker container. It targets scientists, not developers, and provides plugin mechanisms to extend or modify the default behavior.

## Data Storage

* [Cassandra](https://github.com/mesosphere/cassandra-mesos) is a performant and highly available distributed database. Linear scalability and proven fault-tolerance on commodity hardware or cloud infrastructure make it the perfect platform for mission-critical data.
* [ElasticSearch](https://github.com/mesosphere/elasticsearch-mesos) is a distributed search engine. Mesos makes it easy to run and scale.
* [Hypertable](https://code.google.com/p/hypertable/wiki/Mesos) is a high performance, scalable, distributed storage and processing system for structured and unstructured data.
