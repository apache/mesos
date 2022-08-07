---
title: Apache Mesos - Software Projects Built on Mesos
layout: documentation
---
# Software Projects Built on Mesos

## DevOps tooling

* [Vamp](http://vamp.io) is a deployment and workflow tool for container orchestration systems, including Mesos/Marathon. It brings canary releasing, A/B testing, auto scaling and self healing through a web UI, CLI and REST API.

## Long Running Services

* [Aurora](http://aurora.apache.org) is a service scheduler that runs on top of Mesos, enabling you to run long-running services that take advantage of Mesos' scalability, fault-tolerance, and resource isolation.
* [Compose](https://github.com/AVENTER-UG/mesos-compose) is a scheduler (HTTP API) for long running Mesos tasks. The syntax is compatible to docker-compose.
* [M3s](https://github.com/AVENTER-UG/mesos-m3s) is a scheduler (HTTP API) to run multiple K3s (Kubernetes) cluster on top of Mesos.
* [Marathon](https://github.com/mesosphere/marathon) is a private PaaS built on Mesos. It automatically handles hardware or software failures and ensures that an app is "always on".
* [Singularity](https://github.com/HubSpot/Singularity) is a scheduler (HTTP API and web interface) for running Mesos tasks: long running processes, one-off tasks, and scheduled jobs.
* [SSSP](https://github.com/mesosphere/sssp) is a simple web application that provides a white-label "Megaupload" for storing and sharing files in S3.

## Big Data Processing

* [Apache Airflow provider](https://github.com/AVENTER-UG/airflow-provider-mesos) is a scheduler to scale out Apache Airflow DAG's on Mesos.
* [Cray Chapel](https://github.com/nqn/mesos-chapel) is a productive parallel programming language. The Chapel Mesos scheduler lets you run Chapel programs on Mesos.
* [Dpark](https://github.com/douban/dpark) is a Python clone of Spark, a MapReduce-like framework written in Python, running on Mesos.
* [Exelixi](https://github.com/mesosphere/exelixi) is a distributed framework for running genetic algorithms at scale.
* [Flink](https://ci.apache.org/projects/flink/flink-docs-release-1.3/setup/mesos.html) is an open source platform for distributed stream and batch data processing.
* [Hadoop](https://github.com/mesos/hadoop) Running Hadoop on Mesos distributes MapReduce jobs efficiently across an entire cluster.
* [Hama](http://wiki.apache.org/hama/GettingStartedMesos) is a distributed computing framework based on Bulk Synchronous Parallel computing techniques for massive scientific computations e.g., matrix, graph and network algorithms.
* [MPI](https://github.com/mesosphere/mesos-hydra) is a message-passing system designed to function on a wide variety of parallel computers.
* [Spark](http://spark.incubator.apache.org/) is a fast and general-purpose cluster computing system which makes parallel jobs easy to write.
* [Storm](https://github.com/mesos/storm) is a distributed realtime computation system. Storm makes it easy to reliably process unbounded streams of data, doing for realtime processing what Hadoop did for batch processing.

## Batch Scheduling

* [Chronos](https://github.com/mesos/chronos) is a distributed job scheduler that supports complex job topologies. It can be used as a more fault-tolerant replacement for Cron.
* [Cook](https://github.com/twosigma/cook) is a job scheduler like Torque that not only supports individual tasks, but also Spark. Cook provides powerful automatic preemption and multitenancy features for shared clusters, in order to guarantee throughput to all users while allowing individuals to temporarily "burst" to additional resources as needed. Cook provides a simple REST API & Java client for interaction.
* [Elastic-Job-Cloud](https://github.com/dangdangdotcom/elastic-job) is a distributed scheduled job cloud solution designed with HA and fault-tolerance in mind. It focuses on horizontal scaling, and provides transient and daemon jobs, event and schedule based job triggers, job dependencies, and job history.
* [GoDocker](https://bitbucket.org/osallou/go-docker) is a batch computing job scheduler like SGE, Torque, etc. It schedules batch computing tasks via webui, API or CLI for system or LDAP users, mounting their home directory or other shared resources in a Docker container. It targets scientists, not developers, and provides plugin mechanisms to extend or modify the default behavior.
* [Jenkins](https://github.com/jenkinsci/mesos-plugin) is a continuous integration server. The mesos-jenkins plugin allows it to dynamically launch workers on a Mesos cluster depending on the workload.
* [JobServer](http://www.grandlogic.com/content/html_docs/jobserver.html) is a distributed job scheduler and processor  which allows developers to build custom batch processing Tasklets using point and click web UI.
* [Rhythm](https://github.com/mlowicki/rhythm) is a cron-like job scheduler supporting ACL and Vault for secrets management.

## Data Storage

* [Alluxio](http://alluxio.org) is a memory-centric distributed storage system enabling reliable data sharing at memory-speed across cluster frameworks.
* [Cassandra](https://github.com/mesosphere/cassandra-mesos) is a performant and highly available distributed database. Linear scalability and proven fault-tolerance on commodity hardware or cloud infrastructure make it the perfect platform for mission-critical data.
* [Ceph](https://github.com/vivint-smarthome/ceph-on-mesos) is a resilient, auto-healing, general purpose, open-source distributed storage solution. It provides mountable block storage, object storage API (S3 / Swift APIs supported), and a distributed file system (CephFS). While the framework is young, Ceph itself is mature and there are multitudes of large scale deployments.
* [ElasticSearch](https://github.com/mesos/elasticsearch) is a distributed search engine. Mesos makes it easy to run and scale.
* [Hypertable](https://code.google.com/p/hypertable/wiki/Mesos) is a high performance, scalable, distributed storage and processing system for structured and unstructured data.
* [MrRedis](https://github.com/mesos/mr-redis) MrRedis is a Mesos framework for provisioning [Redis](http://redis.io/) in-memory cache instances. The scheduler provides auto Redis master election, auto recovery of Redis slaves and comes with the CLI and a UI.

## Machine Learning

* [TFMesos](https://github.com/douban/tfmesos) is a lightweight framework to help running distributed [Tensorflow](https://www.tensorflow.org/) Machine Learning tasks on Apache Mesos with GPU support.

## Load Balancing

* [Traefik Mesos provider](https://github.com/AVENTER-UG/traefik-mesos) is a modern HTTP reverse proxy and load balancer with build-in TCP and UDP support.

