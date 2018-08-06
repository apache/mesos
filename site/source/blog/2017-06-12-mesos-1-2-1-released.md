---
layout: post
title: Apache Mesos 1.2.1 Released
permalink: /blog/mesos-1-2-1-released/
published: true
post_author:
  display_name: Adam
tags: Release
---

The latest Mesos 1.2.x release, 1.2.1, is now available for [download](http://mesos.apache.org/downloads). This release includes important bug fixes and improvements on top of 1.2.0. It is recommended to use this version if you are considering using Mesos 1.2. More specifically, this release includes the following:

* [MESOS-1987](https://issues.apache.org/jira/browse/MESOS-1987) - Add support for SemVer build and prerelease labels to stout.
* [MESOS-5028](https://issues.apache.org/jira/browse/MESOS-5028) - Copy provisioner cannot replace directory with symlink.
* [MESOS-5172](https://issues.apache.org/jira/browse/MESOS-5172) - Registry puller cannot fetch blobs correctly from http Redirect 3xx urls.
* [MESOS-6327](https://issues.apache.org/jira/browse/MESOS-6327) - Large docker images causes container launch failures: Too many levels of symbolic links.
* [MESOS-6951](https://issues.apache.org/jira/browse/MESOS-6951) - Docker containerizer: mangled environment when env value contains LF byte.
* [MESOS-6976](https://issues.apache.org/jira/browse/MESOS-6976) - Disallow (re-)registration attempts by old agents.
* [MESOS-7133](https://issues.apache.org/jira/browse/MESOS-7133) - mesos-fetcher fails with openssl-related output.
* [MESOS-7197](https://issues.apache.org/jira/browse/MESOS-7197) - Requesting tiny amount of CPU crashes master.
* [MESOS-7208](https://issues.apache.org/jira/browse/MESOS-7208) - Persistent volume ownership is set to root when task is running with non-root user.
* [MESOS-7210](https://issues.apache.org/jira/browse/MESOS-7210) - HTTP health check doesn't work when mesos runs with --docker_mesos_image.
* [MESOS-7232](https://issues.apache.org/jira/browse/MESOS-7232) - Add support to auto-load /dev/nvidia-uvm in the GPU isolator.
* [MESOS-7237](https://issues.apache.org/jira/browse/MESOS-7237) - Enabling cgroups_limit_swap can lead to "invalid argument" error.
* [MESOS-7261](https://issues.apache.org/jira/browse/MESOS-7261) - maintenance.html is missing during packaging.
* [MESOS-7263](https://issues.apache.org/jira/browse/MESOS-7263) - User supplied task environment variables cause warnings in sandbox stdout.
* [MESOS-7264](https://issues.apache.org/jira/browse/MESOS-7264) - Possibly duplicate environment variables should not leak values to the sandbox.
* [MESOS-7265](https://issues.apache.org/jira/browse/MESOS-7265) - Containerizer startup may cause sensitive data to leak into sandbox logs.
* [MESOS-7272](https://issues.apache.org/jira/browse/MESOS-7272) - Unified containerizer does not support docker registry version < 2.3.
* [MESOS-7280](https://issues.apache.org/jira/browse/MESOS-7280) - Unified containerizer provisions docker image error with COPY backend.
* [MESOS-7316](https://issues.apache.org/jira/browse/MESOS-7316) - Upgrading Mesos to 1.2.0 results in some information missing from the `/flags` endpoint.
* [MESOS-7346](https://issues.apache.org/jira/browse/MESOS-7346) - Agent crashes if the task name is too long.
* [MESOS-7350](https://issues.apache.org/jira/browse/MESOS-7350) - Failed to pull image from Nexus Registry due to signature missing.
* [MESOS-7366](https://issues.apache.org/jira/browse/MESOS-7366) - Agent sandbox gc could accidentally delete the entire persistent volume content.
* [MESOS-7368](https://issues.apache.org/jira/browse/MESOS-7368) - Documentation of framework role(s) in proto definition is confusing.
* [MESOS-7383](https://issues.apache.org/jira/browse/MESOS-7383) - Docker executor logs possibly sensitive parameters.
* [MESOS-7389](https://issues.apache.org/jira/browse/MESOS-7389) - Mesos 1.2.0 crashes with pre-1.0 Mesos agents.
* [MESOS-7400](https://issues.apache.org/jira/browse/MESOS-7400) - The mesos master crashes due to an incorrect invariant check in the decoder.
* [MESOS-7427](https://issues.apache.org/jira/browse/MESOS-7427) - Registry puller cannot fetch manifests from Amazon ECR: 405 Unsupported.
* [MESOS-7429](https://issues.apache.org/jira/browse/MESOS-7429) - Allow isolators to inject task-specific environment variables.
* [MESOS-7453](https://issues.apache.org/jira/browse/MESOS-7453) - glyphicons-halflings-regular.woff2 is missing in WebUI.
* [MESOS-7464](https://issues.apache.org/jira/browse/MESOS-7464) - Recent Docker versions cannot be parsed by stout.
* [MESOS-7471](https://issues.apache.org/jira/browse/MESOS-7471) - Provisioner recover should not always assume 'rootfses' dir exists.
* [MESOS-7478](https://issues.apache.org/jira/browse/MESOS-7478) - Pre-1.2.x master does not work with 1.2.x agent.
* [MESOS-7484](https://issues.apache.org/jira/browse/MESOS-7484) - VersionTest.ParseInvalid aborts on Windows.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.2.1)

### Upgrades

Rolling upgrades from a Mesos 1.2.0 cluster to Mesos 1.2.1 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.2.1 from 1.1.x or 1.0.x.

NOTE: In Mesos 1.2.1, the master will no longer allow 0.x agents to register. Interoperability between 1.1+ masters and 0.x agents has never been supported;
however, it was not explicitly disallowed, either. Starting with this release of Mesos, registration attempts by 0.x Mesos agents will be ignored.

### Try it out

Please try out this release and let us know what you think. If you run into any issues, let us know on the [user mailing list and/or Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 16 contributors who made 1.2.1 possible:

Aaron Wood, Adam B, Alexander Rukletsov, Anand Mazumdar, Andrew Schwartzmeyer, Anindya Sinha, Benjamin Bannier, Benjamin Mahler, Chun-Hung Hsiao, Deshi Xiao, Gilbert Song, Haosdent Huang, Jie Yu, Kevin Klues, Neil Conway, and Till Toenshoff.
