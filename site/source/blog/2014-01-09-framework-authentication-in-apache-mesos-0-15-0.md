---
layout: post
title: Mesos 0.15 and Authentication Support
permalink: /blog/framework-authentication-in-apache-mesos-0-15-0/
published: true
post_author:
  display_name: Vinod Kone
  gravatar: 24bc66008e50fb936e696735e99d7815
  twitter: vinodkone
tags: Authentication, Security, Release
---

With the latest Mesos release, [0.15.0](http://mesos.apache.org/downloads/), we are pleased to report that we’ve added Authentication support for frameworks (see [MESOS-418](https://issues.apache.org/jira/browse/MESOS-418)) connecting to Mesos. In a nutshell, this feature allows only authenticated frameworks to register with Mesos and launch tasks. Authentication is important as it prevents rogue frameworks from causing problems that may impact the usage of resources within a Mesos cluster.

## How it works

- **SASL**

    Mesos uses the [Cyrus SASL library](http://asg.web.cmu.edu/sasl/) to provide authentication. SASL is a very flexible authentication framework that allows two endpoints to authenticate with each other and also has support for various authentication mechanisms (ANONYMOUS, PLAIN, CRAM-MD5, GSSAPI etc).

    In this release, Mesos uses SASL with the CRAM-MD5 authentication mechanism. The process for enabling authentication begins with the creation of an authentication credential that is unique to the framework. This credential constitutes a **principal** and **secret** pair, where **principal** refers to the identity of the framework. Note that the **principal** is different from the framework **user** (the Unix user which executors run as) or the resource **role** (role that has reserved certain resources on a slave). These credentials should be shipped to the Mesos master machines, as well as given access to the framework, meaning that both the framework and the Mesos masters should be started with these credentials.

    Once authentication is enabled, Mesos masters only allow authenticated frameworks to register. Authentication for frameworks is performed under the hood by the new scheduler driver.

For specific instructions on how to do this please read the [upgrade](http://mesos.apache.org/documentation/latest/upgrades/) instructions.

## Looking ahead

- **Adding authentication support for slaves**

    Similar to adding authentication support to frameworks, it would be great to add authentication support to the slaves. Currently any node in the network can run a Mesos slave process and register with the Mesos master. Requiring slaves to authenticate with the master before registration would prevent rogue slaves from causing problems (e.g., DDoSing the master, getting access to users tasks etc) in the cluster.

- **Integrating with Kerberos**

   Currently the authentication support via shared secrets between frameworks and masters is basic to benefit usability. To improve upon this basic approach, a more powerful solution would be to integrate with an industry standard authentication service like [Kerberos](http://en.wikipedia.org/wiki/Kerberos_(protocol)). A nice thing about SASL and one of the reasons we picked it is because of its support for integration with GSSAPI/Kerberos. We plan to leverage this support to integrate Kerberos with Mesos.

- **Data encryption**

    Authentication is only part of the puzzle when it comes to deploying and running applications securely in the cloud. Another crucial component is data encryption. Currently all the messages that flow through the Mesos cluster are un-encrypted making it possible for intruders to intercept and potentially control your task. We plan to add encryption support by adding SSL support to [libprocess](https://github.com/3rdparty/libprocess), the low-level communication library that Mesos uses which is responsible for all network communication between Mesos components.

- **Authorization**

    We are also investigating authorizing principals to allow them access to only a specific set of operations like launching tasks or using resources. In fact, you could imagine a world where an authenticated **principal** will be authorized to on behalf of a subset of **user**s and **role**s for launching tasks and accepting resources respectively. This authorization information could be stored in a directory service like LDAP.

### Thanks

While a lot of people contributed to this feature, we would like to give special thanks to [Ilim Igur](https://twitter.com/ilimugur), our [Google Summer of Code](http://www.google-melange.com/gsoc/homepage/google/gsoc2013)  intern who started this project and contributed to the intial design and implementation.

If you are as excited as us about this feature please go ahead and play with [Mesos 0.15.0](http://mesos.apache.org) and let us know what you think. You can get in touch with us via [our mailing lists or IRC](http://mesos.apache.org/community/).