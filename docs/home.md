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
* [Agent Recovery](agent-recovery.md) for doing seamless agent upgrades and allowing executors to survive `mesos-agent` crashes.
* [Authentication](authentication.md)
* [Authorization](authorization.md)
* [Configuration](configuration.md) and [CMake configuration](configuration-cmake.md) for command-line arguments.
* [Container Image](container-image.md) for supporting container images in Mesos containerizer.
* [Containerizers](containerizers.md) for containerizer overview and use cases.
  * [Containerizer Internals](containerizer-internals.md) for implementation details of containerizers.
  * [Docker Containerizer](docker-containerizer.md) for launching a Docker image as a Task, or as an Executor.
  * [Mesos Containerizer](mesos-containerizer.md) default containerizer, supports both Linux and POSIX systems.
    * [CNI support](cni.md)
    * [Docker Volume Support](docker-volume.md)
* [Framework Rate Limiting](framework-rate-limiting.md)
* [Task Health Checking](health-checks.md)
* [High Availability](high-availability.md) for running multiple masters simultaneously.
* [HTTP Endpoints](endpoints/) for available HTTP endpoints.
* [Logging](logging.md)
* [Maintenance](maintenance.md) for performing maintenance on a Mesos cluster.
* [Monitoring](monitoring.md)
* [Operational Guide](operational-guide.md)
* [Roles](roles.md)
* [SSL](ssl.md) for enabling and enforcing SSL communication.
* [Nested Container and Task Group (Pod)](nested-container-and-task-group.md)
* [Tools](tools.md) for setting up and running a Mesos cluster.
* [Upgrades](upgrades.md) for upgrading a Mesos cluster.
* [Weights](weights.md)
* [Windows Support](windows.md) for the state of Windows support in Mesos.


## Advanced Features

* [Attributes and Resources](attributes-resources.md) for how to describe the agents that comprise a cluster.
* [Fetcher Cache](fetcher.md) for how to configure the Mesos fetcher cache.
* [Multiple Disks](multiple-disk.md) for how to allow tasks to use multiple isolated disk resources.
* [Networking](networking.md)
  * [Container Network Interface (CNI)](cni.md)
  * [Port Mapping Isolator](port-mapping-isolator.md)
* [Nvidia GPU Support](gpu-support.md) for how to run Mesos with Nvidia GPU support.
* [Oversubscription](oversubscription.md) for how to configure Mesos to take advantage of unused resources to launch "best-effort" tasks.
* [Persistent Volume](persistent-volume.md) for how to allow tasks to access persistent storage resources.
* [Quota](quota.md) for how to configure Mesos to provide guaranteed resource allocations for use by a role.
* [Replicated Log](replicated-log-internals.md) for information on the Mesos replicated log.
* [Reservation](reservation.md) for how operators and frameworks can reserve resources on individual agents for use by a role.
* [Shared Resources](shared-resources.md) for how to share persistent volumes between tasks managed by different executors on the same agent.


## APIs
* [API Client Libraries](api-client-libraries.md) lists client libraries for the HTTP APIs.
* [Doxygen](/api/latest/c++/namespacemesos.html) documents the C++ API.
* [Executor HTTP API](executor-http-api.md) describes the new HTTP API for communication between executors and the Mesos agent.
* [Javadoc](/api/latest/java/) documents the old Java API.
* [Operator HTTP API](operator-http-api.md) describes the new HTTP API for communication between operators and Mesos master/agent.
* [Scheduler HTTP API](scheduler-http-api.md) describes the new HTTP API for communication between schedulers and the Mesos master.
* [Versioning](versioning.md) describes HTTP API and release versioning.


## Running Mesos Frameworks

* [Mesos frameworks](frameworks.md) for a list of apps built on top of Mesos and instructions on how to run them.
* [Sandbox](sandbox.md) describes a useful debugging arena for most users.


## Developing Mesos Frameworks

* [Designing Highly Available Mesos Frameworks](high-availability-framework-guide.md)
* [Developer Tools](tools.md) for hacking on Mesos or writing frameworks.
* [Framework Development Guide](app-framework-development-guide.md) describes how to build applications on top of Mesos.
* [Reconciliation](reconciliation.md) for ensuring a framework's state remains eventually consistent in the face of failures.


## Extending Mesos

* [Allocation Modules](allocation-module.md) for how to write custom resource allocators.
* [Mesos Modules](modules.md) for specifying Mesos modules for master, agent and tests.


## Contributing to Mesos

* [Committers and Maintainers](committers.md) a listing of project committers and component maintainers; useful when seeking feedback.
* [Committing](committing.md) guidelines for committing changes.
* [Development Roadmap](roadmap.md)
* [Documentation Guide](documentation-guide.md)
  * [C++ Style Guide](c++-style-guide.md)
  * [Doxygen Style Guide](doxygen-style-guide.md)
  * [Markdown Style Guide](markdown-style-guide.md)
* [Doxygen](/api/latest/c++/) documents the internal Mesos APIs.
* [Effective Code Reviewing](effective-code-reviewing.md) guidelines, tips, and learnings for how to do effective code reviews.
* [Engineering Principles and Practices](engineering-principles-and-practices.md) to serve as a shared set of project-level values for the community.
* [Release Guide](release-guide.md)
* [Reopening a Review](reopening-reviews.md) for our policy around reviving reviews on ReviewBoard.
* [Reporting an Issue, Improvement, or Feature](reporting-a-bug.md) for getting started with JIRA.
* [Submitting a Patch](submitting-a-patch.md) for getting started with ReviewBoard and our tooling around it.
* [Testing Patterns](testing-patterns.md) for tips and tricks used in Mesos tests.
* [Working groups](working-groups.md) a listing of groups working on different components.


## More Info about Mesos

* [Academic Papers and Project History](https://www.usenix.org/conference/nsdi11/mesos-platform-fine-grained-resource-sharing-data-center)
* [Design docs](design-docs.md) list of design documents for various Mesos features
* [Powered by Mesos](powered-by-mesos.md) lists organizations and software that are powered by Apache Mesos.


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
