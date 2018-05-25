---
title: Apache Mesos - Resource Provider
layout: documentation
---

# Resource Provider

Resource provider is a new abstraction introduced in Mesos 1.5. Leveraging this,
the resource-providing part of Mesos can be easily extended and customized.
Before 1.5, this part of the logic is hard-coded in the agent. Resource
providers are mainly responsible for updating Mesos about available resources
and handling operations on those resources.

There are two types of resource providers: Local Resource Providers (LRP) and
External Resource Providers (ERP). Local resource providers only provide
resources that are tied to a particular agent node, while external resource
providers provide resources that are not tied to any agent node (a.k.a. global
resources). The resource provider API is designed in such a way that it works
for both types of resource providers. In Mesos 1.5, only local resource
providers are supported.

The resource provider API is an HTTP-based API, allowing resource providers to
be running outside the Mesos master or agent. This is important for ERPs.

There is a component in the agent, called the Resource Provider Manager, that
monitors and manages LRPs on that agent. The same component will be running in
the master in the future to monitor ERPs.
