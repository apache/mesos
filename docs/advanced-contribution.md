---
title: Apache Mesos - Advanced Contribution Guidelines
layout: documentation
---

# Advanced Contribution Guidelines

## Prerequisites

If you'd like to make significant contributions to Mesos, then you'll want to become familiar with the standard Mesos development workflow. In addition to the dependencies needed to build Mesos (detailed in [building](building.md)), a couple other things are necessary:

* Required account authorizations
    + Apache Review Board Account
    + Apache JIRA Account
<br/><br/>
* Required installations
    + RBtools (0.7.10 is known to work, 1.0.1 appears to have an issue)
    + [pre-commit](https://pre-commit.com/#install)

## Issue Tracking, Code Review, and Mailing Lists

* We use [Apache Software Foundation JIRA](https://issues.apache.org/jira/browse/mesos/) to track and manage issues. If you don't already have an account, you'll need to create one.

* We use [Apache Review Board](https://reviews.apache.org) for code reviews.

  **NOTE**: Apache Review Board currently does not allow creation of new accounts. We are aware of this and are looking into possible solutions;
  in the meantime please coordinate with your shepherd on how to best transmit your patches.

    * A code review request should be created for every JIRA that involves a change to the codebase.
* Once your JIRA and Review Board accounts are in place please go ahead and create a review or GitHub pull request with an entry for yourself in [contributors.yaml](https://github.com/apache/mesos/blob/master/docs/contributors.yaml) file.
    * The file is used to map contributions to the JIRA and Review Board accounts of their respective authors. We also use this mapping to track contributions of various organizations to Apache Mesos.
    * Feel free to omit the `affiliations` field out in case you do not want your contribution to be atributed to a particular organization.
    * In the `email` field please specify the email that your local git client is setup with.
* Joining the following mailing lists will help you stay up-to-date on Mesos development:
    * Developer list: [dev-subscribe@mesos.apache.org](mailto:dev-subscribe@mesos.apache.org)
    * Issue list: [issues-subscribe@mesos.apache.org](mailto:issues-subscribe@mesos.apache.org)
    * Review list: [reviews-subscribe@mesos.apache.org](mailto:reviews-subscribe@mesos.apache.org)
    * Build list: [builds-subscribe@mesos.apache.org](mailto:builds-subscribe@mesos.apache.org) respectively.

## The Contribution Process

Here is the standard procedure for proposing and making changes to Mesos:

### Before Coding Starts

1. Find a JIRA issue that is currently unassigned that you want to work on at [JIRA issue tracker](https://issues.apache.org/jira/browse/MESOS), or create your own (you'll need a JIRA account for this, see above)!
    1. This could be a JIRA representing a bug (possibly a bug that you encountered and reported, e.g. when trying to build) or a new feature.
    2. Prefer working on issues marked as "[Accepted](https://issues.apache.org/jira/browse/MESOS-1?jql=project%20%3D%20MESOS%20AND%20status%20%3D%20Accepted)", rather than merely "Open". If an issue has been accepted, it means at least one Mesos developer thought that the ideas proposed in the issue are worth pursuing further.
    3. Issues marked with the "[newbie](https://issues.apache.org/jira/browse/MESOS-1?jql=project%20%3D%20MESOS%20AND%20status%20%3D%20Accepted%20AND%20labels%20%3D%20newbie)" label can be good candidates for "starter" projects. You can also look for the labels "newbie++", "beginner", and "beginners".
    4. When identifying a JIRA issue to work on, it is recommended to work on items that are relevant to the next release. Selecting work items important for the next release increases the priority for reviewers during the contribution process. See the tracking ticket for the release to figure out the high priority projects or ask the release manager to guide you.
2. Assign the JIRA to yourself.
    1. You will be able to assign the JIRA to yourself as soon as a JIRA admin updates your account to 'contributor' status. This will usually be done soon after your pull request with additions to the contributors.yaml file is merged.
3. Formulate a plan for resolving the issue. Guidelines to consider when designing a solution can be found in the [effective-code-reviewing](effective-code-reviewing.md) document. It is important to discuss your proposed solution within the JIRA ticket early in the resolution process in order to get feedback from reviewers. Early discussions will help:
    1. ensure the solution will be scoped in a consumable fashion;
    2. eliminate duplicate work with other contributions; and
    3. alert anyone interested in following the activity and progress of the ticket.
4. Find a **shepherd** to collaborate on your patch. A shepherd is a Mesos committer that will work with you to give you feedback on your proposed design, and to eventually commit your change into the Mesos source tree. To find a shepherd, you can do one or more of the following:
    1. Email the dev mailing list (include a link to your JIRA issue).
    2. Add a comment to your JIRA issue, referencing by username one or more Mesos [committers](committers.md) whom you believe would be interested in shepherding. The listed maintainers of the portion of the codebase you're working on are good candidates to reach out to.
    3. Email potential shepherds directly.
    3. Ask the developers on Mesos Slack or on IRC (in the [mesos channel](irc://irc.freenode.net/mesos) on [Freenode](https://freenode.net)).

### Create your patch

1. Create one or more test cases to exercise the bug or the feature (the Mesos team uses [test-driven development](http://en.wikipedia.org/wiki/Test-driven_development)). Before you start coding, make sure these test cases all fail.
    1. The [testing patterns](testing-patterns.md) page has some suggestions for writing test cases.
2. Make your changes to the code (using whatever IDE/editor you choose) to actually fix the bug or implement the feature.
    1. Before beginning, please read the [Mesos C++ Style Guide](c++-style-guide.md). It is recommended to use the git pre-commit hook to automatically check for style errors. The hooks are set up by invoking `./support/setup_dev.sh`.
    2. Most of your changes will probably be to files inside of `BASE_MESOS_DIR`
    3. From inside of the root Mesos directory: `./bootstrap` and `./support/setup-dev.sh`.
    4. To build, we recommend that you don't build inside of the src directory. We recommend you do the following:
        1. From inside of the root Mesos directory: `mkdir build && cd build`
        2. `../configure`
        3. `make`
        4. Now all of the files generated by the build process will be contained in the build directory you created, instead of being spread throughout the src directory, which is a bit messier. This is both cleaner, and makes it easy to clean up if you want to get rid of the files generated by `configure` and `make`. I.e. You can reset your build process without risking changes you made in the src directory, by simply deleting the build directory, and creating a new one.
3. Make sure that all of the unit tests pass, including the new test cases you have added: `make check`.
    1. To build all tests without executing them, use something like: `make tests`.
    2. To execute a single unit test (helpful when trying to debug a test case failure), use something like: `make check GTEST_FILTER="HTTPTest.Delete"`.
    3. If you added new tests, make sure you run them repeatedly in order to catch inconsistent failures. A command like the following can be used to run an individual test 1000 times:
```
    sudo GLOG_v=1 ./bin/mesos-tests.sh --verbose --gtest_filter="*DOCKER*" --gtest_break_on_failure --gtest_repeat=1000
```
4. Divide your change into one or more Git commits. Each commit should represent a single logical (atomic) change to the Mesos source code: this makes your changes easier to review. For more information, see the [reviewer guidelines](effective-code-reviewing.md).
    1. Try to avoid including other, unrelated cleanups (e.g., typo fixes or style nits) in the same commit that makes functional changes. While typo fixes are great, including them in the same commit as functional changes makes the commit history harder to read.
    2. Developers often make incremental commits to save their progress when working on a change, and then "rewrite history" (e.g., using `git rebase -i`) to create a clean set of commits once the change is ready to be reviewed.
    3. Commit messages should be in past tense. The first sentence should summarize the change; it should start with a capital letter, not exceed 72 characters and end in a period.
5. Make sure to pull in any changes that have been committed to master branch. Using Git, do this via something like:
    1. `git checkout master`
    2. `git pull`
    3. `git checkout my_branch`
    4. Check the output of `git diff master` and make sure it lists only your changes. If other changes you did not make are listed, try `git rebase master` to bring your branch up to date with master.

### Submit your patch

1. You're ready to submit your patch for review!
    1. Log in or create an account at [Apache Review Board](http://reviews.apache.org).
    2. The easiest (and recommended) way to submit reviews is through `post-reviews.py` a wrapper around post-review.
    3. First, install RBTools (0.7.10 is recommended, 1.0.1 appears to have an issue). [See Instructions](https://www.reviewboard.org/docs/rbtools/dev/).
    4. Configure post-review. The easiest method is to symlink to the sample config: `ln -s support/reviewboardrc .reviewboardrc`.
    5. Log into Review Board from the command line: run `rbt status`.
    6. From your local branch run `support/post-reviews.py`.
    7. Note that `post-reviews.py` creates a new review for every commit on your branch that is different from the `master`.
    8. Be sure to add your JIRA issue id (e.g. MESOS-1) to the field labeled "Bugs" (this will automatically link).
    9. Add your shepherd under the "People" field, in the "Reviewers" section. You should also include other Mesos community members who have contributed to the discussion of your proposed change.
    10. Under "Description" in addition to details about your changes, include a description of any documentation pages that need to be added, or are affected by your changes (e.g. did you change or add any configuration options/flags? Did you add a new binary?)
    11. Under "Testing Done", explain what new tests you have created, what tests were modified, and what procedures you went through to test your changes.
2. Wait for a code review from another Mesos developer via Review Board, address their feedback and upload updated patches until you receive a "Ship It" from a Mesos committer.
    1. If you don't receive any feedback, contact your shepherd to remind them. While the committers try their best to provide prompt feedback on proposed changes, they are busy and sometimes a patch gets overlooked.
    2. When addressing feedback, adjust your existing commit(s) instead of creating new commits, otherwise `post-reviews.py` will create a new review (`git rebase -i` is your friend).
    3. Review Board comments should be used for code-specific discussions, and JIRA comments for bigger-picture design discussions.
    4. Always respond to each RB comment that you address directly (i.e. each comment can be responded to directly) with either "Done." or a comment explaining how you addressed it.
    5. If an issue has been raised in the review, please resolve the issue as "Fixed" or "Dropped". If "Dropped" please add a comment explaining the reason. Also, if your fix warrants a comment (e.g., fixed differently than suggested by the reviewer) please add a comment.
3. After consensus is reached on your JIRA/patch, you're review request will receive a "Ship It!" from a committer, and then a committer will commit your patch to the git repository. Congratulations and thanks for participating in our community!
4. The last step is to ensure that the necessary documentation gets created or updated so the whole world knows about your new feature or bug fix.

## Advanced JIRA Tickets

As you gain experience contributing to Mesos you may want to tackle more advanced JIRA tickets. These items may touch multiple components within Mesos and/or may have a significant impact on the developer or user community. In these cases, a working group of stakeholders is formed to develop a design document. The initial formation of this working group will be part of the community communication resources, e.g. the re-occurring developer sync meetings, the developer email list, the IRC channel, etc. For reference, a contributor new to an advanced level work item can refer to the work done for the [inverse offer](https://issues.apache.org/jira/browse/MESOS-1592) project.

## Style Guides

For patches to the core, we ask that you follow the [Mesos C++ Style Guide](c++-style-guide.md).

The [Mesos Developer Guide](developer-guide.md) contains some best practices and design patterns that are useful for new developers to learn.

## Additional Guidance

The following links provide additional guidance as you get started contributing to Apache Mesos.

## Core Libraries

There are two core libraries in Mesos: *stout* and *libprocess*. *stout* is the low level operating system abstraction tooling that should be used in place of writing your own low level tools. *libprocess* is a library used to support building compatible concurrent components. New contributors to Mesos should become familiar with these libraries and utilize them where appropriate. Additional documentation can be found in the following two README files: [3rdparty/libprocess/README.md](https://github.com/apache/mesos/blob/master/3rdparty/libprocess/README.md) and [3rdparty/stout/README.md](https://github.com/apache/mesos/blob/master/3rdparty/stout/README.md).
