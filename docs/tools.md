---
title: Apache Mesos - Tools
layout: documentation
---

# Tools

## Ops Tools

These tools make it easy to set up and run a Mesos cluster.

* [collectd plugin](https://github.com/rayrod2030/collectd-mesos) to collect Mesos cluster metrics.
* [Snap plugin](https://github.com/intelsdi-x/snap-plugin-collector-mesos) for collecting Mesos cluster metrics and statistics about running executors.
* [Deploy scripts](deploy-scripts.md) for launching a Mesos cluster on a set of machines.
* [Chef Cookbook by Anthony Caiafa](https://github.com/acaiafa/mesos) Install Mesos and configure master and agent. This cookbook currently supports installation from the Mesosphere packages.
* [Chef cookbook by Everpeace](https://github.com/everpeace/cookbook-mesos) Install Mesos and configure master and agent. This cookbook supports installation from source or the Mesosphere packages.
* [Chef cookbook by Mdsol](https://github.com/mdsol/mesos_cookbook) Application cookbook for installing the Apache Mesos cluster manager. This cookbook installs Mesos via packages provided by Mesosphere.
* [Puppet Module by Deric](https://github.com/deric/puppet-mesos) This is a Puppet module for managing Mesos nodes in a cluster.
* [Vagrant setup by Everpeace](https://github.com/everpeace/vagrant-mesos) Spin up your Mesos Cluster with Vagrant!
* [Vagrant setup by Mesosphere](https://github.com/mesosphere/playa-mesos) Quickly build Mesos sandbox environments using Vagrant.
* [Vagrant and Ansible setup by WooRank](https://github.com/Woorank/vagrant-mesos-cluster) Mesos, Marathon, Docker, HAProxy and service discovery, using Ansible and Vagrant.
* [Vagrant and Puppet setup by Tayzlor](https://github.com/tayzlor/vagrant-puppet-mesosphere) Build a Mesos Cluster with Vagrant and Puppet (and optionally Consul.io)!

## Developer Tools

If you want to hack on Mesos or write a new framework, these tools will help.

* [clang-format](clang-format.md) to automatically apply some of the style rules dictated by the [Mesos C++ Style Guide](c++-style-guide.md).
* [Go Bindings and Examples](https://github.com/mesosphere/mesos-go) Write a Mesos framework in Go! Comes with an example scheduler and executor.
* [Mesos Framework giter8 Template](https://github.com/mesosphere/scala-sbt-mesos-framework.g8) This is a giter8 template. The result of applying this template is a bare-bones Apache Mesos framework in Scala using SBT for builds and Vagrant for testing on a singleton cluster.
* [Scala Hello World](https://gist.github.com/guenter/7471695) A simple Mesos "Hello World": downloads and starts a web server on every node in the cluster.
* [Xcode Workspace](https://github.com/tillt/xcode-mesos) Hack on Mesos in Xcode.

Can't find yours in the list? Please submit a patch, or email user@mesos.apache.org and we'll add you!
