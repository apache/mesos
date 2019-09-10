---
layout: post
title: "Apache Mesos 1.9: Agent Draining, Quota Limit and Security Improvements"
permalink: /blog/mesos-1-9-0-released/
published: true
post_author:
  display_name: Qian Zhang & Gilbert Song
tags: Release
---

We are excited to announce that Apache Mesos 1.9.0 is now available for [download](/downloads). Please take a look at what's new in this release!

# New Features and Improvements

## Agent Draining

Automatic agent draining was added to allow operators to prepare agent nodes for maintenance without requiring schedulers to implement support for the feature. Since the pre-existing maintenance primitives offered by Mesos require that schedulers make changes, some operators have had difficulty using them effectively in clusters containing frameworks which have not done so. When automatic draining is initiated on an agent, all tasks are gracefully killed, and operators can monitor the master's state to detect when draining on the agent is complete.

Agent deactivation and reactivation primitives were also added to the master API, allowing operators to stop and resume offers from particular agents. Used in concert with framework-specific APIs, this new functionality enables operators to perform manual draining of agent nodes in cases where greater control is desired.

## Resource Management

Prior to Mesos 1.9, the quota related APIs only exposed quota "guarantees" which ensured a minimum amount of resources would be available to a role. Setting guarantees also set implicit quota limits. In Mesos 1.9.0, quota limits are now exposed directly.

* Quota guarantees are now deprecated in favor of using only quota limits. Enforcement of quota guarantees required that Mesos holds back enough resources to meet all of the unsatisfied quota guarantees. Since Mesos is moving towards an optimistic offer model (to improve multi-role / multi- scheduler scalability, see MESOS-1607), it will become no longer possible to enforce quota guarantees by holding back resources. In such a model, quota limits are simple to enforce, but quota guarantees would require a complex "effective limit" propagation model to leave space for unsatisfied guarantees.

* For these reasons, quota guarantees, while still functional in Mesos 1.9, are now deprecated. A combination of limits and priority based preemption will be simpler in an optimistic offer model.

## Containerization

A number of containerization-related improvements have landed in Mesos 1.9.0:

* The Mesos containerizer now supports configurable IPC namespace and /dev/shm. Container can be configured to have a private IPC namespace and /dev/shm or share them from its parent, and the size of its private /dev/shm is also configurable.

* A new `/containerizer/debug` HTTP endpoint has been added. This endpoint exposes debug information for the Mesos containerizer. At the moment, it returns a list of pending operations related to isolators and launchers.

* A new Linux NNP (No New Privs) isolator has been added to the Mesos Containerizer. The isolator allows configuration of the [no_new_privs](https://www.kernel.org/doc/Documentation/prctl/no_new_privs.txt) flag for launched containers. The `no_new_privs` flag disables the ability of container tasks to acquire additional privileges by means of executing a child process e.g. through invocation of `setuid` or `setgid` programs. The flag is configurable on the agent and provides additional depth of security for containerized processes.

* A new `--docker_ignore_runtime` flag has been added. This causes the agent to ignore any runtime configuration present in Docker images.

* The Mesos containerizer now includes ephemeral overlayfs storage in the task disk quota as well as sandbox storage.

## Improved Security for TLS Connections

Since Mesos 0.23, Mesos had support for using TLS [1] to encrypt the communication to and from Mesos components - the same protocol that secures `https`, `smtps`, and many others. Roughly speaking, every time a TLS client connects to a TLS server, that server will present a certificate signed by a trusted certificate authority which is used to verify the identity of the server.

In Mesos, this behaviour is controlled by the environment variables `LIBPROCESS_SSL_VERIFY_CERT` and `LIBPROCESS_SSL_REQUIRE_CERT`. The former would do the cryptographic verification **if** a certificate was supplied, and the latter would reject all connections where no certificate was presented. This may sound straightforward, but this behaviour has proven challenging for Mesos operators, with many leaving TLS verification disabled in practice. The reason for that is that Mesos components are acting as both TLS client and server at the same time.

Enabling server certificate validation in this scenario had the effect of requiring **all** incoming connections to present valid client certificates. This put an additional burden on operators to build infrastructure to distribute valid client certificates to all users of Mesos endpoints.

With Mesos 1.9, we updated the semantics of both flags to be more aligned with the needs of Mesos operators:

* `LIBPROCESS_SSL_VERIFY_CERT` now only applies to *server certificates*, which are always required for TLS connections. If it is set to true, the server certificate is verified for all outgoing connections.

* `LIBPROCESS_SSL_REQUIRE_CERT` now only applies to *client certificates*: If it is set to true, all incoming connections must present a valid client certificate.

By switching to the OpenSSL-provided API for hostname validation [2], we are able to improve security and make the behaviour more uniform across different platforms. We were also able to eliminate reverse DNS lookups while establishing a connection which improves reliability and performance.

[1] https://en.wikipedia.org/wiki/Transport_Layer_Security

[2] http://mesos.apache.org/documentation/latest/ssl/#libprocess_ssl_hostname_validation_scheme-legacy-openssl-default

# Upgrade

Upgrades from Mesos 1.8.0 to Mesos 1.9.0 should be straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.9.0.

# Community

Inspired by the work that went into this release? Want to get involved? Have feedback? We'd love to hear from you! Join a [working group](http://mesos.apache.org/community/#working-groups) or start a conversation in the [community](http://mesos.apache.org/community/)!

# Thank you!

Thanks to the 28 contributors who made Mesos 1.9.0 possible:

Alexander Rukletsov, Andrei Budnik, Andrei Sekretenko, Armand Grillet, Bartosz Galek, Benjamin Bannier, Benjamin Mahler, Benno Evers, Bilal Amarni, Chun-Hung Hsiao, Gastón Kleiman, Gilbert Song, Greg Mann, Hans Beck, Jacob Janco, James Peach, James Wright, Jan Schlicht, Joseph Wu, Meng Zhu, Pavel Kirillov, Qian Zhang, Stéphane Cottin, Till Toenshoff, Tomasz Janiszewski, Vinod Kone, Zhitao Li, Fei Long
