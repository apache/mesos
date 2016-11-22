---
title: Apache Mesos - Documentation Home
layout: documentation
---

# Documentation

## Mesos Fundamentals

* [Mesos Architecture](architecture.md) providing an overview of Mesos concepts.
* [Video and Slides of Mesos Presentations](presentations.md)

## Running Mesos

* [Getting Started](getting-started.md) for basic instructions on compiling and installing Mesos.
* [CMake](cmake.md) for building Mesos with CMake.
* [Upgrades](upgrades.md) for upgrading a Mesos cluster.
* [Configuration](configuration.md) for command-line arguments.
* [HTTP Endpoints](endpoints/) for available HTTP endpoints.
* [Containerizer](containerizer.md) for containerizer overview and use cases.
  * [Containerizer Internals](containerizer-internals.md) for implementation details of containerizers.
  * [Mesos Containerizer](mesos-containerizer.md) default containerizer, supports both Linux and POSIX systems.
    * [Docker Volume Support](docker-volume.md)
    * [CNI support](cni.md)
  * [Docker Containerizer](docker-containerizer.md) for launching a Docker image as a Task, or as an Executor.
* [Roles](roles.md)
* [Weights](weights.md)
* [Authentication](authentication.md)
* [Authorization](authorization.md)
* [Framework Rate Limiting](framework-rate-limiting.md)
* [Logging](logging.md)
* [High Availability](high-availability.md) for running multiple masters simultaneously.
* [Operational Guide](operational-guide.md)
* [Monitoring](monitoring.md)
* [Agent Recovery](agent-recovery.md) for doing seamless agent upgrades and allowing executors to survive `mesos-agent` crashes.
* [Maintenance](maintenance.md) for performing maintenance on a Mesos cluster.
* [Tools](tools.md) for setting up and running a Mesos cluster.
* [SSL](ssl.md) for enabling and enforcing SSL communication.
* [Container Image](container-image.md) for supporting container images in Mesos containerizer.
* [Windows Support](windows.md) for the state of Windows support in Mesos.

## Advanced Features

* [Attributes and Resources](attributes-resources.md) for how to describe the agents that comprise a cluster.
* [Fetcher Cache](fetcher.md) for how to configure the Mesos fetcher cache.
* [Networking](networking.md)
  * [Container Network Interface (CNI)](cni.md)
  * [Port Mapping Isolator](port-mapping-isolator.md)
* [Nvidia GPU Support](gpu-support.md) for how to run Mesos with Nvidia GPU support.
* [Oversubscription](oversubscription.md) for how to configure Mesos to take advantage of unused resources to launch "best-effort" tasks.
* [Persistent Volume](persistent-volume.md) for how to allow tasks to access persistent storage resources.
* [Multiple Disks](multiple-disk.md) for how to to allow tasks to use multiple isolated disk resources.
* [Quota](quota.md) for how to configure Mesos to provide guaranteed resource allocations for use by a role.
* [Reservation](reservation.md) for how operators and frameworks can reserve resources on individual agents for use by a role.
* [Replicated Log](replicated-log-internals.md) for information on the Mesos replicated log.
* [Shared Resources](shared-resources.md) for how to share persistent volumes between tasks managed by different executors on the same agent.

## APIs
* [Scheduler HTTP API](scheduler-http-api.md) describes the new HTTP API for communication between schedulers and the Mesos master.
* [Executor HTTP API](executor-http-api.md) describes the new HTTP API for communication between executors and the Mesos agent.
* [Operator HTTP API](operator-http-api.md) describes the new HTTP API for communication between operators and Mesos master/agent.
* [API Client Libraries](api-client-libraries.md) lists client libraries for the HTTP APIs.
* [Versioning](versioning.md) describes HTTP API and release versioning.
* [Javadoc](/api/latest/java/) documents the old Java API.
* [Doxygen](/api/latest/c++/namespacemesos.html) documents the C++ API.

## Running Mesos Frameworks

* [Mesos frameworks](frameworks.md) for a list of apps built on top of Mesos and instructions on how to run them.
* [Sandbox](sandbox.md) describes a useful debugging arena for most users.

## Developing Mesos Frameworks

* [Framework Development Guide](app-framework-development-guide.md) describes how to build applications on top of Mesos.
* [Designing Highly Available Mesos Frameworks](high-availability-framework-guide.md)
* [Reconciliation](reconciliation.md) for ensuring a framework's state remains eventually consistent in the face of failures.
* [Developer Tools](tools.md) for hacking on Mesos or writing frameworks.

## Extending Mesos

* [Mesos Modules](modules.md) for specifying Mesos modules for master, agent and tests.
* [Allocation Modules](allocation-module.md) for how to write custom resource allocators.

## Contributing to Mesos

* [Reporting an Issue, Improvement, or Feature](reporting-a-bug.md) for getting started with JIRA.
* [Submitting a Patch](submitting-a-patch.md) for getting started with ReviewBoard and our tooling around it.
* [Reopening a Review](reopening-reviews.md) for our policy around reviving reviews on ReviewBoard.
* [Testing Patterns](testing-patterns.md) for tips and tricks used in Mesos tests.
* [Effective Code Reviewing](effective-code-reviewing.md) guidelines, tips, and learnings for how to do effective code reviews.
* [Engineering Principles and Practices](engineering-principles-and-practices.md) to serve as a shared set of project-level values for the community.
* [Committing](committing.md) guidelines for committing changes.
* [Committers and Maintainers](committers.md) a listing of project committers and component maintainers; useful when seeking feedback.
* [Working groups](working-groups.md) a listing of groups working on different components.
* [Doxygen](/api/latest/c++/) documents the internal Mesos APIs.
* [Documentation Guide](documentation-guide.md)
  * [C++ Style Guide](c++-style-guide.md)
  * [Doxygen Style Guide](doxygen-style-guide.md)
  * [Markdown Style Guide](markdown-style-guide.md)
* [Development Roadmap](roadmap.md)
* [Release Guide](release-guide.md)

## More Info about Mesos

* [Powered by Mesos](powered-by-mesos.md) lists organizations and software that are powered by Apache Mesos.
* [Academic Papers and Project History](https://www.usenix.org/conference/nsdi11/mesos-platform-fine-grained-resource-sharing-data-center)
* [Design docs](design-docs.md) list of design documents for various Mesos features

## Books on Mesos

<div class="row">
  <div class="col-xs-6 col-md-4">
    <a href="https://www.packtpub.com/big-data-and-business-intelligence/apache-mesos-essentials" class="thumbnail">
      <img src="https://www.packtpub.com/sites/default/files/9781783288762.png" alt="Apache Mesos Essentials by Dharmesh Kakadia">
    </a>
    <p class="text-center">Apache Mesos Essentials by Dharmesh Kakadia (Packt, 2015)</p>
  </div>
  <div class="col-xs-6 col-md-4">
    <a href="http://shop.oreilly.com/product/0636920039952.do" class="thumbnail">
      <img src="http://akamaicovers.oreilly.com/images/0636920039952/lrg.jpg" alt="Building Applications on Mesos by David Greenberg">
    </a>
    <p class="text-center">Building Applications on Mesos by David Greenberg (O'Reilly, 2015)</p>
  </div>
  <div class="col-xs-6 col-md-4">
    <a href="https://www.packtpub.com/big-data-and-business-intelligence/mastering-mesos" class="thumbnail">
      <img src="https://www.packtpub.com/sites/default/files/6249OS_5186%20Mastering%20Mesos.jpg" alt="Mastering Mesos by Dipa Dubhashi and Akhil Das">
    </a>
    <p class="text-center">Master Mesos by Dipa Dubhashi and Akhil Das (Packt, 2016)</p>
  </div>
  <div class="col-xs-6 col-md-4">
    <a href="https://www.manning.com/books/mesos-in-action" class="thumbnail">
      <img src="https://images.manning.com/255/340/resize/book/d/62f5c9b-0946-4569-ad50-ffdb84876ddc/Ignazio-Mesos-HI.png" alt="Mesos in Action by Roger Ignazio">
    </a>
  <p class="text-center">Mesos in Action by Roger Ignazio (Manning, 2016)
  </div>
</div>
