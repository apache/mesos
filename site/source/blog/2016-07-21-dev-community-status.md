---
layout: post
title: "Mesos Developer Community Status Report"
permalink: /blog/dev-community-status/
published: true
post_author:
  display_name: Michael Park
  gravatar: 2ab9cab3a7cf782261c583c1f48a81b0
  twitter: mcypark
tags: Community, MesosCon
---

The Mesos project has a large and rapidly growing community of users and contributors.
In this post we would like to reflect on the current state
of the community as well as speak about some benefits and challenges that the
growth brings.

One of such challenges is that the we have to make sure that everyone’s interests
are met. This isn’t always easy due to occasional conflicts of interest. For example,
developers are eager to see their patches vetted and committed to the codebase as
rapidly as possible, whereas production users of Mesos want to make sure that the stability of the software is never
compromised for the sake of rolling out new features. Both are essential components
of a healthy open source infrastructure project, and we want to strike a harmonizing
balance.

Transparency is the key for being able to grow the community while also making
sure that participants understand and agree with the development direction. In
this post - much of which derives from a recent [MesosCon 2016 presentation] -
we are going to present some statistics that reflect the current state of the
community participation. The time frame for the following statistics ranges from
the 0.24 release (July 2015) until 1.0.0-rc2 (June/July 2016).

We mined the data for this post from Mesos [git log](https://git1-us-west.apache.org/repos/asf?p=mesos.git;a=tree;f=docs;h=b8c6cfa978ea6986fa7e9181f209448f4984a3ab;hb=9c8bfa9b1bfe52fa6b44aaf883333311bdde5519) and used the recently added
[contributors.yaml](https://git1-us-west.apache.org/repos/asf?p=mesos.git;a=blob;f=docs/contributors.yaml;h=e17fed49ea358837eb33827dc3aceea9cd69c1b3;hb=9c8bfa9b1bfe52fa6b44aaf883333311bdde5519) file for mapping contributors to organizations.

### Community Growth and Diversity
<%= image_tag 'blog/2016-developer-community-1.png' %>

The number of unique contributors has been increasing steadily over time. As of
this writing, the number of contributors for the most recent release is greater
than 100. This is a more than factor two improvement over the 0.24 release for
which there were about 50 unique contributors!

Historically there have always been large organizations from which the significant
amount of contributions came from. In recent times, Mesosphere, IBM and Microsoft have
been leading in terms of number of contributions made to the codebase. However,
in terms of numbers of people, the individual contributors greatly outnumber those
from Mesosphere, IBM and Microsoft.

This is a great milestone for all of our individual contributors out there!
We greatly appreciate your contribution to the project and we're looking forward
to seeing even more commits from individual contributors in the future.

<%= image_tag 'blog/2016-developer-community-2.png' %>

In terms of numbers of commits from an organization, Mesosphere has the highest number after
contributing 62.8% of the commits. It is followed by 'Other' (which is a default organization
for all the authors who do not have an affiliation entry in the [contributors.yaml](https://git1-us-west.apache.org/repos/asf?p=mesos.git;a=blob;f=docs/contributors.yaml;h=e17fed49ea358837eb33827dc3aceea9cd69c1b3;hb=9c8bfa9b1bfe52fa6b44aaf883333311bdde5519)), IBM and Microsoft. We are actively working on involving more members from the community
and are looking forward to seeing an even greater community participation.

It is important to note that 1.0 release has been in the making for a longer time
than previous releases, and that is why there is a significant spike in the number of
commits for 1.0-rc2.
Here is how the same graph looks like once we change the breakdown from releases to months.

<%= image_tag 'blog/2016-developer-community-3.png' %>

It presents a fuller picture where the upward trend is still there, but one can also notice
that the smooth growth trend has been there for a while.

The last graph that we want to present shows the breakdown of commits per release
between contributors and the PMC members (aka Mesos Committers). As one can see
the number of commits authored by non-committers vastly outnumbers the ones originating
from committers. This shows that the active committers spend a lot of time shepherding
and landing patches from contributors.

<%= image_tag 'blog/2016-developer-community-4.png' %>

Going forward we are going to introduce a page on our website where we will continuously
publish up-to-date statistics around the developer community. We are also going to double
down on initiatives to improve the contribution experience on the Mesos project. We
started by doing a spring cleaning on our GitHub and Reviewboard backlogs, as well as the Kanban
board in JIRA. This will make sure that shepherds do not get drowned in stale reviews, and that
everyone can see what the community is working on. We also have some ideas around improving the
developer tools, collecting and analyzing metrics around reviews, as well as some other initiatives
that should foster a greater participation. If you are interested in helping us improve the
community participation and drive some of these initiatives please join the 'Community' working group.

Stay tuned for more news on the dev list!

[MesosCon 2016 presentation]: https://www.youtube.com/watch?list=PLGeM09tlguZQVL7ZsfNMffX9h1rGNVqnC&v=SnPmU61fVjQ
