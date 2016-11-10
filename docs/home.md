---
title: Apache Mesos - Documentation Home
layout: documentation
---

# Documentation

## Understanding Mesos
* [Overview](./concepts/understanding-mesos-overview.md)
* [Quick Start: Get Your Mesos Cluster Up and Running](./concepts/quick-start-get-mesos-up-and-running.md)
  * Quickly run a cluster to help you understand Mesos. {To be written - please contribute.}
* [What is Mesos? How does it relate to the world?](./concepts/what-is-mesos.md):
  * How does it fit into a typical DC software stack? {To be written - please contribute.}
  * How can it be used to solve typical problems faced by DC operators/users? {To be written - please contribute.}
  * How does it relate to Marathon, Aurora, Cook, Kubernetes, etc.? {To be written - please contribute.}
* [Concepts](architecture.md)  <To be restructured, as follows.>
  * [What components make up “Mesos” and how do they interrelate?](mesos-components.md)
    * Masters and agents {To be written - please contribute.}
    * Schedulers and executors {To be written - please contribute.}
    * Offers {To be written - please contribute.}
    * Containers and isolation {To be written - please contribute.}
  * [Resource Management vs Cluster Manager vs Container Orchestration](./concepts/resource-mgmt-cluster-mgr-container-orchstn.md)
    * A comparison of these three important elements {To be written - please contribute.}

## Downloading, Installing, and Building Mesos
* [Overview](./concepts/downloading-building-installing-overview.md)
  * Reference “Quick Start: Get Your Mesos Cluster Up and Running {To be written - please contribute.}
  * Role-based workflow illustration {To be written - please contribute.}
* [Installing Mesos](getting-started.md) <To be restructured, as follows.>
    * [CentOS 6.6](./tasks/intallation-centos-6.6.md)
    * [CentOS 7.1](./tasks/intallation-centos-7.1.md)
    * [Mac OS X](./tasks/intallation-mac-os-x.md)
    * [Ubuntu 14.04 LTS](./tasks/intallation-ubuntu-14.04.md)
    * [Ubuntu 16.04 LTS](./tasks/intallation-ubuntu-16.04.md)
    * [Windows](./tasks/intallation-windows.md)
    * [Posix (Building Mesos)](./tasks/intallation-posix.md)
    * [Using Vagrant](./tasks/intallation-vagrant.md)
    * [Using DC/OS (cloud)](./tasks/intallation-dcos.md)
    * [Using ACS](./tasks/intallation-acs.md)(an easy way to play around with a Mesos cluster - use for quickstart?)
* [Intallation Tools](tools.md)
* [Frameworks](frameworks.md): a list of apps built on top of Mesos and instructions on how to run them.
* [Release notes](upgrades.md)

## Operating Mesos
* [Overview](./concepts/operating-mesos-overview.md)
  * Role-based workflow illustration. {To be written - please contribute.}
* [Configure Mesos](./concepts/configure-mesos.md)
  * [Operational Guide](operational-guide.md)*
  * [Choosing a Type of Deployment](./concepts/choosing-a-type-of-deployment.md)
  * [Tools](tools.md)
  * [Configure Masters](./tasks/configure-masters.md)
    * [Configuration Options](configuration.md)
  * [Configure Agents](./tasks/configure-agents.md)
  * [Configure Operating Systems](./tasks/configure-operating-systems.md)
  * [Configuration Options](./references/configuration-options.md)
    * Both {To be written - please contribute.}
    * Master only {To be written - please contribute.}
    * Agent only {To be written - please contribute.}
* [Masters](./concepts/masters.md)
  * [Single Master](./concepts/single-master.md)
  * [Multiple High-Availability Masters](high-availability.md)
    * [Replicated Log](replicated-log-internals.md): information on the Mesos replicated log.
  * [Observability](./concepts/observability.md)
    * [Logging](logging.md)
    * [Monitoring and Metrics](monitoring.md)
    * [Configuration Options](configuration.md)
  * [Framework Rate Limiting](framework-rate-limiting.md)
* [Agents](./concepts/agents.md)
  * [Recovery](agent-recovery.md): Do seamless agent upgrades and allow executors to survive `mesos-agent` crashes.
  * [Sandbox](sandbox.md) describes a useful debugging arena for most users.
  * [Attributes](attributes-resources.md)
  * [Resources (Discovery and Creation)](attributes-resources.md)
    * CPU {To be written - please contribute.}
    * Memory {To be written - please contribute.}
    * [GPU](gpu-support.md)
  * [Agent Isolators](./concepts/agent-isolators.md)
  * [Configuration Options](configuration.md)
* [Modules](modules.md): Specifying Mesos modules for master, agent and tests.
  * tbd {To be written - please contribute.}
* [Resource Allocation](allocation-module.md)
  * [DRF](./concepts/drf.md)
  * [Roles](roles.md)
  * [Weights](weights.md)
  * [Quotas](quota.md): how to configure Mesos to provide guaranteed resource allocations for use by a role.
  * [Oversubscription](oversubscription.md): how to configure Mesos to take advantage of unused resources to launch "best-effort" tasks.
  * [Reservations](reservation.md): how operators and frameworks can reserve resources on individual agents for use by a role.
  * [Disk resources](./concepts/disk-resources.md)
* [Performing Maintenance](maintenance.md)
  * [Upgrade](upgrades.md) a running Mesos cluster.
* [Security and Access Control](./concepts/security-and-access-control.md)
* [Authentication](authentication.md)
* [Authorization](authorization.md)
  * [SSL](ssl.md)

## Using Containers
* [Overview](./concepts/using-containers-overview.md)
  * Role-based workflow illustration <To be illustrated - please contribute.>
* [Tools](tools.md)
  * tbd {To be written - please contribute.}
* [Docker Volumes](docker-volume.md)
* [Run a container using Mesos executor](./tasks/run-a-container-using-mesos-executor.md)
* [Containerizer](containerizer.md) overview and use cases.
  * [Container Images](container-image.md)
* [Fetching](fetcher.md)
* [Provisioning](./concepts/provisioning.md)
* [Metrics](monitoring.md)
* [Logging](logging.md)
* [Networking](networking.md)
  * [Container Network Interface (CNI)](cni.md)
  * [Port Mapping Isolator](port-mapping-isolator.md)
* [Storage](./concepts/storage.md)
  * [Persistent Local Volumes](persistent-volume.md): Allow tasks to access persistent storage resources
  [Shared Resources](shared-resources.md): Share persistent volumes between tasks managed by different executors on the same agent
  * [Persistent External Volumes](./concepts/persistent-external-volumes.md)
  * [Multiple Disks](multiple-disk.md): how to to allow tasks to use multiple isolated disk resources
* [GPU support](gpu-support.md) (Nvidia)

## Developing Mesos Frameworks
* [Overview](./concepts/developing-mesos-frameworks-overview.md)
  * Role-based workflow illustration. <To be illustrated - please contribute.>
  * [Framework Development Guide](app-framework-development-guide.md)
* [Schedulers](./concepts/developing-schedulers.md)
* [Executors](./concepts/developing-executors.md)
  * Command Executor {To be written - please contribute.}
  * Custom Executors {To be written - please contribute.}
* [Resource Offers](./concepts/developing-resource-offers.md)
* [Allocation Modules](allocation-module.md): how to write custom resource allocators.
* [Advanced Resource Types](./concepts/developing-advanced-resource-types.md)
  * Reservations {To be written - please contribute.}
  * Persistent Volumes {To be written - please contribute.}
  * GPUs {To be written - please contribute.}
* [High Availability](high-availability-framework-guide.md)
  * [Reconciliation](reconciliation.md): Ensuring a framework's state remains eventually consistent in the face of failures
* [Containers](./concepts/developing-containers.md)
  * [Mesos Containerizer](./concepts/developing-mesos-containerizer.md)
    * [Images](container-image.md)
    * Disks {To be written - please contribute.}
    * Networking {To be written - please contribute.}
  * [Containerizer](containerizer.md) overview and use cases.
    * [Containerizer Internals](containerizer-internals.md): implementation details of containerizers.
    * [Mesos Containerizer](mesos-containerizer.md) default containerizer, supports both Linux and POSIX systems.
      * [Docker Volume Support](docker-volume.md)
      * [CNI support](cni.md)
    * [Docker Containerizer](docker-containerizer.md): launching a Docker image as a Task, or as an Executor.
* [Tools](tools.md)


## Mesos APIs
* [Overview](./concepts/mesos-apis-overview.md)
* [API Concepts (Architecture)](./concepts/api-concepts.md)
* [Authentication and Authorization](./concepts/authentication-and-authorization.md)
* [Scheduler HTTP API](scheduler-http-api.md) describes the new HTTP API: communication between schedulers and the Mesos master.
* [Executor HTTP API](executor-http-api.md) describes the new HTTP API: communication between executors and the Mesos agent.
* [Operator HTTP API](operator-http-api.md) describes the new HTTP API: communication between operators and Mesos master/agent.

* [HTTP Endpoints](endpoints/)
* [API Client Libraries](api-client-libraries.md): the HTTP APIs
* [Versioning](versioning.md): HTTP API and releases
* [Javadoc](/api/latest/java/) documents the old Java API
* [Doxygen](/api/latest/c++/) documents the internal Mesos APIs.
* [Doxygen](/api/latest/c++/namespacemesos.html) documents the C++ API

## Contributing to Mesos
* [Overview](./concepts/contributing-to-mesos-overview.md)
  * Role-based workflow illustration <To be illustrated - please contribute.>
* [Design Documents](./concepts/design-documents.md)
* [Developer Community](./concepts/developer-community.md)
  * [How To Get Involved](./concepts/how-to-get-involved.md)
    * [Reporting Issues](reporting-a-bug.md)
    * [Proposing Changes](reporting-a-bug.md)
    * [Submitting a Patch](submitting-a-patch.md)
    * [Reopening a Review](reopening-reviews.md)
    * [Effective Code Reviewing](effective-code-reviewing.md) guidelines, tips, and learnings for how to do effective code reviews.
    * [Committing](committing.md) guidelines for committing changes.
  * [Committers and Maintainers](committers.md) a listing of project committers and component maintainers; useful when seeking feedback.
  * [Working groups](working-groups.md) a listing of groups working on different components.
* [Development Practices](./concepts/development-practices.md)
  * [Engineering Principles](engineering-principles-and-practices.md) to serve as a shared set of project-level values for the community.
  * [C++ Style Guide](./concepts/cpp-style-guide.md)
  * [Testing Patterns](testing-patterns.md): tips and tricks used in Mesos tests.
  * [Documentation Guide](documentation-guide.md)
    * [C++ Style Guide](c++-style-guide.md)
    * [Doxygen Style Guide](doxygen-style-guide.md)
    * [Markdown Style Guide](markdown-style-guide.md)

  * [Code Reviewing Guidelines](./concepts/code-reviewing-guidelines.md)
  * [Committing Guidelines](./concepts/committing-guidelines.md)
  * [Development Roadmap](roadmap.md)
  * [Release Guide](release-guide.md)


## Mesos Internals
* [Overview](./concepts/mesos-internals-overview.md)
* [Fetcher Cache Internals](fetcher-cache-internals.md)
* [Containerizer Internals](containerizer-internals.md)



## More Information about Mesos
* [Overview](./concepts/more-info-about-mesos-overview.md)
* [Video and Slides of Mesos Presentations](presentations.md)
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
    <a href="https://www.manning.com/books/mesos-in-action" class="thumbnail">
      <img src="https://images.manning.com/255/340/resize/book/d/62f5c9b-0946-4569-ad50-ffdb84876ddc/Ignazio-Mesos-HI.png" alt="Mesos in Action by Roger Ignazio">
    </a>
  <p class="text-center">Mesos in Action by Roger Ignazio (Manning, 2016)
  </div>
</div>

## Dog Pound (where topics go while they’re looking for a home)

* [Windows Support](windows.md): the state of Windows support in Mesos.
