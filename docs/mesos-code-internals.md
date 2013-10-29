---
layout: documentation
---

# Mesos Code Internals

## Top-level directories in the Mesos distribution

* 3rdparty - Contains necessary open source third party software that Mesos leverages for things such as logging, etc.
* docs - Documentation that's packaged/shipped with each release.
* ec2 - Scripts for launching a Mesos cluster on EC2. See the wiki page on "EC2-Scripts".
* frameworks - Included Mesos Frameworks. See the READMEs in each one. See the [App/Framework development guide](app-framework-development-guide.md) for a crash course in how Mesos Frameworks get resources from the Mesos master.
* include - Contains headers that contain the interfaces that Mesos users need in order to interact with Mesos (e.g. the Mesos Framework API)
* src - Contains the entire Mesos source tree. See below for more details about the directories inside of `src`.

## Mesos source code

The Mesos source code (found in `MESOS_HOME/src`) is organized into the following hierarchy:

* common - Shared source files (such as utilities and data structures).
* deploy
* detector
* examples
* exec
* files
* launcher
* linux
* local
* log
* logging
* master - Source files specific to the mesos-master daemon.
* mesos
* messages
* python
* sasl
* scaling
* sched - Source files specific to the mesos-slave daemon.
* slave
* tests
* webui
* zookeeper