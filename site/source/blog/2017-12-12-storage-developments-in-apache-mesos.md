---
layout: post
title: "Storage developments in Apache Mesos: Interview with Jie Yu"
published: true
post_author:
  display_name: Greg Mann
  gravatar: 20c5c9695f0891b6b093704331238133
  twitter: greggomann
tags: Storage
---

The [Container Storage Interface](https://github.com/container-storage-interface) (CSI) is a community driven effort to standardize the way that containers connect to storage, so that any compliant container platform will be able to use storage from any compliant ecosystem vendor. I recently sat down with long-time Mesos committer Jie Yu, who is participating heavily in the development of CSI, to discuss the effort that he’s been leading to build CSI compatibility into Mesos.
<br>
<br>
**Greg**: Before we get started, could you tell us a bit about your history with the Apache Mesos project?
<br>
**Jie**: I’ve been working on Mesos for 4 years. I started at Twitter, and then came to Mesosphere 2 years ago. I’ve worked on many container-related things in the codebase. This includes - among other things - networking and, more recently, storage.

**Greg**: Could you summarize, at a high level, the direction in which you’re currently taking Mesos storage support?
<br>
**Jie**: There are many storage vendors out there, and we don’t want to build all this vendor-specific code ourselves. Instead, we want a system that allows storage vendors to plug into Mesos using a consistent interface. Then we don’t need to maintain code for each vendor, which requires specific expertise.
Some other projects ask vendors to put their code into the main codebase. This can be painful because then the vendor’s release cycle is tied to the cycle of the project itself. This is hard to manage if you have many vendors. That’s why we decided to go with this “out-of-tree” solution for storage.

**Greg**: What use cases are you most excited to enable?
<br>
**Jie**: Historically, we didn’t support global resources like EBS. We have limited support using [DVDI](https://docs.docker.com/engine/extend/plugins_volume/), but there are issues with this. I think external storage is something I’m really excited about with this work. Being able to plug in arbitrary vendors’ storage systems is exciting.
The other thing I’m excited about is that we have a solution that allows a vendor to provide just a single container image for their plugin, and we’ll take care of the rest of the work end-to-end. So it will be very simple for people to integrate their storage system with Mesos.

**Greg**: When can we expect these features to become available?
<br>
**Jie**: The plan is to have local CSI storage support in Mesos 1.5, then external storage support in 1.6.

**Greg**: How can community members get involved going forward?
<br>
**Jie**: Well, there are multiple avenues. For storage vendors, you can write a CSI plugin and test it with Mesos. For Mesos users, please try out this feature! And if you want to contribute to the coding, you can reach out on the [mailing list](mailto:dev@apache.mesos.org). Hopefully we can organize a storage working group in the near future once the feature lands.

**Greg**: What’s next after Mesos 1.6?
<br>
**Jie**: If we can wrap up our plans for 1.6, then we’ll be in pretty good shape. The next thing is building a great storage ecosystem for Mesos. We can reach out to vendors and try to get them to integrate.

**Greg**: Is there anything else you’d like to share with the community?
<br>
**Jie**: One thing to highlight is that while we’re doing storage work right now, the abstractions we’ve built for storage can be used for other purposes in Mesos - for example, they could be used for other [global resources](https://issues.apache.org/jira/browse/MESOS-2728) like IP addresses. In the cloud, you could use this to manage things like AWS’s ENI.
In the future, we probably need a general interface to support arbitrary devices. The industry trend is the introduction of more and more specialized devices to support things like machine learning. We probably need a general interface for these types of devices so that Mesos can expose them in a unified way.
<br>
<br>
To learn more about CSI work in Mesos, you can dig into the design document [here](https://docs.google.com/document/d/125YWqg_5BB5OY9a6M7LZcby5RSqBwo2PZzpVLuxYXh4/edit?usp=sharing), and track the ongoing progress on [Apache JIRA](https://issues.apache.org/jira/browse/MESOS-7235). To contribute to the multi-platform effort to implement CSI, join the [CSI Community](https://github.com/container-storage-interface/community#community).
