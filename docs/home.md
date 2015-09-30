---
layout: documentation
---

# Documentation

## Mesos Fundamentals

* [Mesos Architecture](/documentation/latest/mesos-architecture/) providing an overview of Mesos concepts.
* [Video and Slides of Mesos Presentations](/documentation/latest/mesos-presentations/)

## Running Mesos

* [Getting Started](/documentation/latest/getting-started/) for basic instructions on compiling and installing Mesos.
* [Configuration](/documentation/latest/configuration/) for command-line arguments.
* [Mesos Containerizer](/documentation/latest/mesos-containerizer/) default containerizer, supports both Linux and POSIX systems.
* [Docker Containerizer](/documentation/latest/docker-containerizer/) for launching a Docker image as a Task, or as an Executor.
* [External Containerizer](/documentation/latest/external-containerizer/) for custom containerization implementations.
* [Framework Authentication](/documentation/latest/authentication/)
* [Framework Authorization](/documentation/latest/authorization/)
* [Framework Rate Limiting](/documentation/latest/framework-rate-limiting/)
* [Logging and Debugging](/documentation/latest/logging-and-debugging/) for viewing Mesos and framework logs.
* [High Availability](/documentation/latest/high-availability/) for running multiple masters simultaneously.
* [Operational Guide](/documentation/latest/operational-guide/)
* [Monitoring](/documentation/latest/monitoring/)
* [Network Monitoring and Isolation](/documentation/latest/network-monitoring/)
* [Slave Recovery](/documentation/latest/slave-recovery/) for doing seamless upgrades.
* [Maintenance](/documentation/latest/maintenance/) for performing maintenance on a Mesos cluster.
* [Tools](/documentation/latest/tools/) for setting up and running a Mesos cluster.
* [SSL](/documentation/latest/mesos-ssl/) for enabling and enforcing SSL communication.

## Advanced Features

* [Attributes and Resources](/documentation/attributes-resources/) for how to describe the slaves that comprise a cluster.
* [Fetcher Cache](/documentation/latest/fetcher/) for how to configure the Mesos fetcher cache.
* [Oversubscription](/documentation/latest/oversubscription/) for how to configure Mesos to take advantage of unused resources to launch "best-effort" tasks.
* [Persistent Volume](/documentation/latest/persistent-volume/) for how to allow tasks to access persistent storage resources.
* [Reservation](/documentation/latest/reservation/) for how to configure Mesos to allow slaves to reserve resources.

## Running Mesos Frameworks

 * [Mesos frameworks](/documentation/latest/mesos-frameworks/) for a list of apps built on top of Mesos and instructions on how to run them.

## Developing Mesos Frameworks

* [Framework Development Guide](/documentation/latest/app-framework-development-guide/) describes how to build applications on top of Mesos.
* [Reconciliation](/documentation/latest/reconciliation/) for ensuring a framework's state remains eventually consistent in the face of failures.
* [Scheduler HTTP API](/documentation/latest/scheduler_http_api/) describes the new HTTP API for communication between schedulers and the Mesos master.
* [Javadoc](/api/latest/java/) documents the Mesos Java API.
* [Doxygen](/api/latest/c++/namespacemesos.html) documents the Mesos C++ API.
* [Developer Tools](/documentation/latest/tools/) for hacking on Mesos or writing frameworks.

## Extending Mesos

* [Mesos Modules](/documentation/latest/modules/) for specifying Mesos modules for master, slave and tests.
* [Allocation Modules](/documentation/latest/allocation-module/) for how to write custom resource allocators.

## Contributing to Mesos

* [Reporting an Issue, Improvement, or Feature](/documentation/latest/reporting-a-bug/) for getting started with JIRA.
* [Submitting a Patch](/documentation/latest/submitting-a-patch/) for getting started with ReviewBoard and our tooling around it.
* [Testing Patterns](/documentation/latest/mesos-testing-patterns/) for tips and tricks used in Mesos tests.
* [Effective Code Reviewing](/documentation/latest/effective-code-reviewing/) guidelines, tips, and learnings for how to do effective code reviews.
* [Engineering Principles and Practices](/documentation/latest/engineering-principles-and-practices/) to serve as a shared set of project-level values for the community.
* [Committing](/documentation/latest/committing/) guidelines for committing changes.
* [Committers and Maintainers](/documentation/latest/committers/) a listing of project committers and component maintainers; useful when seeking feedback.
* [Doxygen](/api/latest/c++/) documents the internal Mesos APIs.
* [Documentation Guide](/documentation/latest/mesos-documentation-guide/)
  * [C++ Style Guide](/documentation/latest/mesos-c++-style-guide/)
  * [Doxygen Style Guide](/documentation/latest/mesos-doxygen-style-guide/)
  * [Markdown Style Guide](/documentation/latest/mesos-markdown-style-guide/)
* [Development Roadmap](/documentation/latest/mesos-roadmap/)
* [Release Guide](/documentation/latest/release-guide/)

## More Info about Mesos

* [Powered by Mesos](/documentation/latest/powered-by-mesos/) lists organizations and software that are powered by Apache Mesos.
* Academic Papers and Project History
