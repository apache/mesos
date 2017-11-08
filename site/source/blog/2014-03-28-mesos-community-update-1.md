---
layout: post
title: "Mesos Community Update #1"
permalink: /blog/mesos-community-update-1/
published: true
post_author:
  display_name: Matt Trifiro
  twitter: mtrifiro
tags: Community
canonical_url: http://mesosphere.io/community/2014/03/20/mesos-community-update-1/
---

_This is a cross post from the [Mesosphere blog](http://mesosphere.io/community/2014/03/20/mesos-community-update-1/)._

Our community is in the midst of a revolution. Mesos is no longer an edge technology, only for the Twitters and Airbnbs of the world; it’s going mainstream and we are at its center. There is more activity on the Apache mailing lists, more commits in the code, more frameworks being developed, more developers building tools around Mesos—and, perhaps most excitingly, there are more companies deploying Mesos into production.

All of this activity makes it harder to keep up!

The Mesosphere team wants to help you keep up with all of the community activity by curating a concise semi-regular update, of which this is the first. We are looking to share the most interesting and important activities in the Mesos community.

### Here are some recent highlights:

* Venture capital icon [Vinod Khosla](http://en.wikipedia.org/wiki/Vinod_Khosla) gave a call-out to Mesos in his 2014 Open Networking Summit keynote, where he waxed about the need for a data center OS. The Mesos portion begins at 24:07 in the [ONS2014 Keynote YouTube video](http://youtu.be/q61VkqZRjck?t=24m7s%20).

* Lab49 software engineer [Anvar Karimson](https://twitter.com/anvarkarimson) re-implemented Stripe’s legendary Capture the Flag (CTF) system on Mesos. Read his blow-by-blow description here: [Running Stripe CTF 2.0 on Mesos](http://karimson.com/posts/ctf-mesos/).

* Community member [Tomas Barton](https://twitter.com/barton_tomas) gave an introductory talk on Mesos at the [InstallFest](http://www.installfest.cz/if14/) in Prague. In his 59-slide talk, he covered everything from workload balancing to Mesos fault tolerance. View his presentation on SlideShare: [Introduction to Apache Mesos](http://www.slideshare.net/tomasbart/introduction-to-apache-mesos).

* On February 27, [Mesos 0.17.0 was released](http://mesos.apache.org/blog/mesos-0-17-0-released-featuring-autorecovery/) and you can [download it here](http://mesosphere.io/downloads/#apache-mesos-0.17.0). The 0.17.0 release features auto-recovery of the replicated log, which enhances Mesos’s high-availability and fault-tolerance. Read more here: [Mesos 0.17.0 release notes](https://issues.apache.org/jira/secure/ReleaseNote.jspa?projectId=12311242&version=12325669)

* On the path toward fully implementing the [Mesos Registrar Design](https://cwiki.apache.org/confluence/display/MESOS/Registrar+Design+Document), we saw progress toward creating [persistence of state information for slaves](https://issues.apache.org/jira/browse/MESOS-764). Placing a small amount of state information in highly-available storage will make recovery of slaves faster and more graceful (this has been [lacking for a while](https://issues.apache.org/jira/browse/MESOS-295)).

* Mesosphere has packaged a pre-release version of Mesos 0.18.0 (release candidate 4) that you can now [download](http://mesosphere.io/downloads/#apache-mesos-0.18.0-rc4). The primary features of the [upcoming 0.18.0 release](https://github.com/apache/mesos/blob/0.18.0-rc4/CHANGELOG) are changes that make it easier to insert pluggable container technologies, like Docker. 0.18.0 foreshadows some pretty interesting Docker integrations with Mesos.

### Upcoming events:

* BBQ at Mesosphere HQ, 6-8 pm, March 28th. Come join us for a low-key BBQ. 145A Hampshire St, San Francisco (near Potrero @ 15th). Swing by and meet [our team](http://mesosphere.io/team/).

* [ApacheCon](http://apacheconnorthamerica2014.sched.org/), April 7-9 in Denver. Come see two Mesos presentations: [Mesos: Elastically Scalable Operations, Simplified](http://apacheconnorthamerica2014.sched.org/event/d83ffc7d7c56620474eac1a2d8f09967) and [Building and Running Distributed Systems using Apache Mesos](http://apacheconnorthamerica2014.sched.org/event/803cb2a6f321ee02957b1c4eb4ebc01c).

We hope to publish these updates at least a few times each month, and we’re interested in your suggestions. Please let us know what you think about the length, content and style, and send us your tips on what to publish each week. Do you have something to share? Write to us at [community@mesosphere.io](mailto:community@mesosphere.io).

_This post was co-authored by Abhishek Parolkar. Follow him on Twitter at [@parolkar](https://twitter.com/parolkar)._
