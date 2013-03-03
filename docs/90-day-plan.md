# 90-day plan

## At a glance

1. Graduate from the Apache Incubator
1. Add two independent committers
1. Involve Mesos in the Google Summer of Code
1. Automate creation of Mac OS X, .rpm, and .deb binary packages

# Details

This 90-day plan aims to bolster the Mesos community. It addresses "Growing a development community and promoting adoption" in the [[Mesos Roadmap]].

Start: March 1st, 2013. End: May 30, 2013.

## Graduate from the Apache Incubator

* Status → incomplete.
* Justification → Mesos must become a [sub]project or risk termination from Apache.
* Who's working on this → (your name here)
* Next steps → Identify people to work on this goal. Add committers. Call a vote.

## Add two independent committers

* Status → incomplete.
* Justification → This is a prerequisite to Mesos graduating from the Apache incubator.
* Who's working on this → (your name here).
* Next steps → Identify people to work on this goal. See "Ideas on how to add independent committers" section, below.

Independent is defined on the [Mesos incubation status page](http://incubator.apache.org/projects/mesos.html).

## Involve Mesos in the Google Summer of Code

* Status → incomplete.
* Justification → This a battle-tested means of attracting new, able developers.
* Who's working on this → (your name here).
* Next steps → Identify people to work on this goal. Define projects. Apply (March 18-29).

## Automate creation of Mac OS X, .rpm, and .deb binary packages

* Status → incomplete.
* Justification → Binary packages greatly increase OSS project approachability.
* Who's working on this → (your name here).
* Next steps → Identify people to work on this goal.

# Acceptance of plan

* Chris Mattmann: (no reply yet).
* Brian McCallister: (no reply yet).
* Tom White: (no reply yet).
* Ali Ghodsi: (no reply yet).
* Benjamin Hindman: (no reply yet).
* Andy Konwinski: (no reply yet).
* Matei Zaharia: (no reply yet).

# Brainstorm / extra material

## Assumptions

Compared to some Open Source projects (the Linux kernel, the Ruby programming
language), Mesos is niche software.

## Ideas on how to add independent committers

* actively seek out new contributors
* make sure potential contributors know about Mesos
* who are potential contributors?
  * current users that can write documentation or code
  * anyone who manages clusters
* how do we reach these potential contributors?
  * publish scientific articles and whitepapers on Mesos
  * talk about Mesos at conferences
  * publish *non*-scientific articles on Mesos
* reward contributors
  * specific thanks / recognition
* spread the word
  * blog, tweet, etc.
  * write up regular (monthly or weekly) updates ([example from the Symfony2 community](http://symfony.com/blog/a-week-of-symfony-317-21-27-january-2013), note that commit summaries are useful, note "They talked about us" section)
  * apply for relevant competitions / awards
  * participate in the Google Summer of Code
  * get on [FLOSS Weekly](http://twit.tv/show/floss-weekly)
  * do more meetups (mentioned in [January 2013 podling report](http://wiki.apache.org/incubator/January2013))
* highlight contributions
  * [example from Symfony2 community](http://symfony.com/blog/new-in-symfony-2-2-autocomplete-on-the-command-line) - contributors can write these themselves!
* organize coding / doc / testing sprints

## Ideas on how to make Mesos more approachable

* What does "approachable" mean?
  * easy to use (see "Regularly build OS-specific binary packages" goal, below)
  * easy to develop
* Visualize approaching Mesos as a potential contributor
  * What would inspire you to try Mesos?
  * What would inspire you to contribute?
* What do you look for when you approach Open Source Software?
* improve documentation
  * cull old/outdated [wiki] pages
* screencasts/demos (especially of most exciting features)
* curate many bite-size tasks for contributors
  * refactoring
  * code cleanup
  * testing
  * documentation
  * DONE. [Vinod Kone suggests](http://mail-archives.apache.org/mod_mbox/incubator-mesos-dev/201301.mbox/%3CCAAkWvAyo6uEu76%3DPjn2ZePOy7ZG4ksHz_AG%3D9P44M2t%2BnOka6A%40mail.gmail.com%3E) perusing [issues with Minor or Trivial priority](https://issues.apache.org/jira/browse/MESOS#selectedTab=com.atlassian.jira.plugin.system.project%3Aissues-panel)
* Idea: use a survey to measure approachability of Mesos
  * how approachable is Mesos compared to other OSS?
  * what would make Mesos more approachable to you?
* mirror git repo to github ([DONE](https://github.com/apache/mesos))
* mirror mesos-dev mailing list to gmane
* add screenshots
* start a Mesos IRC channel
* [find a volunteer to] create a logo (see [MESOS-337](https://issues.apache.org/jira/browse/MESOS-337))
* separate automated/notification emails (from review board, jenkins, etc.) from hand-typed email discussions
* announce supported compiler version(s), and *make sure Mesos compiles with these* (ideally with continuous integration)
* announce supported runtime hardware/configuration(s), and *make sure Mesos runs on these* (ideally with continuous integration)
* reduce sources of truth
  * http://www.mesosproject.org and http://incubator.apache.org/mesos/ look identical, just make one redirect to the other.

## Other ideas

* do more releases (mentioned in [January 2013 podling report](http://wiki.apache.org/incubator/January2013))
* Make Mesos compile with gcc-4.7 (see [MESOS-271](https://issues.apache.org/jira/browse/MESOS-271))
* Improve Mesos evangelization
  * add more to https://github.com/mesos/mesos/wiki/Powered-by-Mesos (AirBnB?)
* get Hadoop customizations upstream
* add all goals in the 90-day plan to JIRA, track them there instead of here
* trademark the name Mesos

## OSS marketing

One aspect of having a successful Open Source project is marketing. To address
this, we can Test, Measure, Act, and Repeat. For example:

* Test → [apply to] participate in the Google Summer of Code (hereafter GSoC)
* Measure → track number of contributors gained
* Act → Gained 2 contributors? Participate in other programs similar to GSoC, host a program like GSoC, plan to participate in GSoC next year.
* Repeat → GOTO Test

## See also

* [The Art of Community](http://www.artofcommunityonline.org/) by Jono Bacon
* [Producing Open Source Software](http://producingoss.com/) by Karl Fogel
* [announcement of this plan](http://mail-archives.apache.org/mod_mbox/incubator-mesos-dev/201302.mbox/%3C511ADF8E.5080700%40gmail.com%3E)

# Conventions for this page

To facilitate future changes to this document (and discussion of same), please
observe the following conventions:

* [Markdown syntax](daringfireball.net/projects/markdown/syntax).
* Try to Wrap lines at 80-characters (exceptions: literal code examples or long
  strings like URLs).
* Use spaces only (no tabs).

Like the meritocracy of Apache, the best ideas win in this document. Anything
without a clear direction or consensus should be discussed and decided on the
mailing list.

## Writing Goals

Goals are, ideally, SMART (specific, measurable, actionable, realistic, and
timely). Excuse the corny acronym, it's just a useful mneumonic.