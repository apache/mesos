---
layout: documentation
---

# Documentation

## Mesos Fundamentals

* [Mesos Architecture](architecture.md) providing an overview of Mesos concepts.
* [Video and Slides of Mesos Presentations](presentations.md)

## Running Mesos

* [Getting Started](getting-started.md) for basic instructions on compiling and installing Mesos.
* [Upgrades](upgrades.md) for upgrading a Mesos cluster.
* [Configuration](configuration.md) for command-line arguments.
* [Containerizer](containerizer.md) for containerizer overview and use cases.
  * [Containerizer Internals](containerizer-internals.md) for implementation details of containerizers.
  * [Mesos Containerizer](mesos-containerizer.md) default containerizer, supports both Linux and POSIX systems.
  * [Docker Containerizer](docker-containerizer.md) for launching a Docker image as a Task, or as an Executor.
  * [External Containerizer](external-containerizer.md) for custom containerization implementations (deprecated).
* [Roles](roles.md)
* [Framework Authentication](authentication.md)
* [Framework Authorization](authorization.md)
* [Framework Rate Limiting](framework-rate-limiting.md)
* [Logging](logging.md)
* [High Availability](high-availability.md) for running multiple masters simultaneously.
* [Operational Guide](operational-guide.md)
* [Monitoring](monitoring.md)
* [Network Monitoring and Isolation](network-monitoring.md)
* [Slave Recovery](slave-recovery.md) for doing seamless upgrades.
* [Maintenance](maintenance.md) for performing maintenance on a Mesos cluster.
* [Tools](tools.md) for setting up and running a Mesos cluster.
* [SSL](ssl.md) for enabling and enforcing SSL communication.
* [Mesos Image Provisioner](mesos-provisioner.md) for provisioning container filesystems from different image formats.

## Advanced Features

* [Attributes and Resources](attributes-resources.md) for how to describe the slaves that comprise a cluster.
* [Fetcher Cache](fetcher.md) for how to configure the Mesos fetcher cache.
* [Networking for Mesos-managed Containers](networking-for-mesos-managed-containers.md)
* [Oversubscription](oversubscription.md) for how to configure Mesos to take advantage of unused resources to launch "best-effort" tasks.
* [Persistent Volume](persistent-volume.md) for how to allow tasks to access persistent storage resources.
* [Quota](quota.md) for how to configure Mesos to provide guaranteed resource allocations for use by a role.
* [Reservation](reservation.md) for how operators and frameworks can reserve resources on individual agents for use by a role.

## Running Mesos Frameworks

* [Mesos frameworks](frameworks.md) for a list of apps built on top of Mesos and instructions on how to run them.
* [Sandbox](sandbox.md) describes a useful debugging arena for most users.

## Developing Mesos Frameworks

* [Framework Development Guide](app-framework-development-guide.md) describes how to build applications on top of Mesos.
* [Designing Highly Available Mesos Frameworks](high-availability-framework-guide.md)
* [Reconciliation](reconciliation.md) for ensuring a framework's state remains eventually consistent in the face of failures.
* [Scheduler HTTP API](scheduler-http-api.md) describes the new HTTP API for communication between schedulers and the Mesos master.
* [Executor HTTP API](executor-http-api.md) describes the new HTTP API for communication between executors and the Mesos agent.
* [Javadoc](/api/latest/java/) documents the Mesos Java API.
* [Doxygen](/api/latest/c++/namespacemesos.html) documents the Mesos C++ API.
* [Developer Tools](tools.md) for hacking on Mesos or writing frameworks.
* [Versioning](versioning.md) describes how Mesos does API and release versioning.

## Extending Mesos

* [Mesos Modules](modules.md) for specifying Mesos modules for master, slave and tests.
* [Allocation Modules](allocation-module.md) for how to write custom resource allocators.

## Contributing to Mesos

* [Reporting an Issue, Improvement, or Feature](reporting-a-bug.md) for getting started with JIRA.
* [Submitting a Patch](submitting-a-patch.md) for getting started with ReviewBoard and our tooling around it.
* [Testing Patterns](testing-patterns.md) for tips and tricks used in Mesos tests.
* [Effective Code Reviewing](effective-code-reviewing.md) guidelines, tips, and learnings for how to do effective code reviews.
* [Engineering Principles and Practices](engineering-principles-and-practices.md) to serve as a shared set of project-level values for the community.
* [Committing](committing.md) guidelines for committing changes.
* [Committers and Maintainers](committers.md) a listing of project committers and component maintainers; useful when seeking feedback.
* [Doxygen](/api/latest/c++/) documents the internal Mesos APIs.
* [Documentation Guide](documentation-guide.md)
  * [C++ Style Guide](c++-style-guide.md)
  * [Doxygen Style Guide](doxygen-style-guide.md)
  * [Markdown Style Guide](markdown-style-guide.md)
* [Development Roadmap](roadmap.md)
* [Release Guide](release-guide.md)

## More Info about Mesos

* [Powered by Mesos](powered-by-mesos.md) lists organizations and software that are powered by Apache Mesos.
* Academic Papers and Project History
