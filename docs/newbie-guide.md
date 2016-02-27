--
title: Apache Mesos - Newbie Guide
layout: documentation
---
# Newbie Guide

## Introduction

The purpose of this document is to give an introduction and overall description of the contribution process for contributors new to Mesos.  This document is intended to extend the existing documentation with a focus on on-boarding contributors to the community as quickly as possible.

# Contribution Process Overview

The figure below shows a high-level process flow of the contribution delivery process for the Mesos open source distribution. Details of the process can be obtained from the [submitting-a-patch] (submitting-a-patch.md) document of the Mesos distribution along with the Getting Started Guidance section of this document.

![Newbie Document](NewbieContributionOverview.jpg)

## Preparation and Build Environment Setup

In order to complete the contribution process you will need to obtain account authorizations and install additional tooling to build the Mesos binaries. These pre-requisites are listed below:

* Required account authorizations
    + Apache Review Board Account
    + Apache JIRA Account

* Required installations
    + Git
    + RBtools
    + GTest
    + GLog

## Download and Build Mesos

In order to begin contributing you will need to download and build the latest stable release of the Mesos distribution. Documentation on the steps to complete this process is described in the Mesos document and link listed above.

## Make Modifications

Make your intended modifications to the source code and then commit the changes to your local build repository. Changes to the source should also include test cases to exercise the modification. Details about this process are found in the Mesos document listed above and Getting Started section below.

## Submit Patch

Once changes are completed and tested submit the patch for review. This process will take the modifications from review submission to committed changes in the Mesos distribution.

# Getting Started Guidance

The following information will help provide additional guidance to execute the steps summarized in Contribution Process Overview described above as well as provide helpful hints when following the [submitting-a-patch](submitting-a-patch.md) document of the Mesos distribution.

## Apache JIRA Account Pre-Requisite

Detailed steps are provided in the Mesos documentation to obtain a JIRA account. Once you have obtained a JIRA account, ensure the additional step to request addition to the list of “contributors” is completed.

## Tooling Pre-Requisites

There is a set of tooling listed below that is required to be installed for the contribution process. These tools will not be described in this document and it is assumed that the reader will seek alternative references to become familiar with each of these tools outside of this document.

* [Git Client](https://git-scm.com/)
* [RBTools](https://www.reviewboard.org/docs/rbtools/dev/)
* [GTest](https://github.com/google/googletest)
* [GLog](https://github.com/google/glog)

## Core Libraries

There are two core libraries in Mesos: *stout* and *libprocess*. *stout* is the low level operating system abstraction tooling that should be used in place of writing your own low level tools. *libprocess* is a library used to support building compatible concurrent components (see [video](https://www.youtube.com/watch?v=5EIiU6R_6KE) for an introduction to the *libprocess* library). New contributors to Mesos should become familiar with these libraries and utilize them where appropriate.  Additional documentation can be found in the following two README files: 3rdparty/libprocess/README.md and 3rdparty/libprocess/3rdparty/stout/README.md.

## Download and Build Mesos

Detailed steps are provided in the [Mesos documentation](getting-started.md) to download and build Mesos. When downloading the Mesos distribution there are 2 options to obtain the Mesos source code. For contributing, ensure that the Git option is used.

## JIRA

[JIRA](http://issues.apache.org/jira/browse/MESOS) is the issue tracking system for all bug fixes and feature additions in Mesos. When contributing to Mesos, all assignments, discussions, and resolution proposals for a specific issue should be documented as comments within the JIRA item.

## Identifying a JIRA Issue

To begin the contribution process, identify a Mesos JIRA issue that is currently unassigned. It is highly recommended to start contributing to beginner level issues and overtime move to advanced level issues. The JIRA issue-tracking system can be filtered based on labels. The following labels can be used to identify beginner level JIRA tickets:

* newbie
* “*newbie++”
* beginner
* beginners

When identifying a JIRA issue to work on, it is recommended to work on items that are relevant to the next release. Selecting work items important for the next release increases the priority for reviewers during the contribution process. See the tracking ticket for the release to figure out the high priority projects or ask the release manager to guide you.

## Assign a JIRA Issue

There are a couple options to assign a work item: 1) create a new JIRA issue to work on or 2) identify an existing JIRA issue item to work on as described above. Whichever option is chosen there are several steps that should be followed when assigning yourself a JIRA ticket.

It is important to identify a shepherd before you assign a ticket to yourself. Working with a shepherd will reveal its priority at the current time. To identify a shepherd look at the [maintainers](committers.md) file to get an idea who to ask to shepherd your JIRA issue.\


## JIRA Issue Solution Proposals
Once you have an assigned JIRA issue and you have identified a shepherd, it is important to discuss your proposed solution within the JIRA ticket early in the resolution process in order to get feedback from reviewers. Early discussions will help:

1. ensure the solution will be scoped in a consumable fashion;

1. eliminate duplicate work with other contributions; and

1. alert anyone interested in following the activity and progress of the ticket.


Guidelines to consider when designing a solution can be found in the [effective-code-reviewing](effective-code-reviewing.md) document.

## Making Changes

After completing the solution review make the source code changes, build and successfully run relevant test cases. Guidelines for these processes can be found in the [submitting-a-patch](submitting-a-patch.md) and [mesos-c++style-guide] (c++-style-guide.md).

When creating and running test cases pay particular attention to race conditions. One recommendation is to run system tests multiple times.  Below is a sample command to repeat the test 100 times.


*sudo GLOG\_v=1 ./bin/mesos-tests.sh --verbose --gtest_filter=”\*DOCKER\*” --break-on-error --gtest_shuffle --gtest_repeat=100*

## Submit a Patch to the Review Board

Submit your patch for review after you have come to an agreement with your shepherd on the proposed solution, made the modifications and successfully ran the test cases. Make sure your shepherd is added as a "reviewer" (among others) in the review. This will ensure that your reviews get processed.

When submitting a patch for review, include all testing done in the submission documentation. Follow the detailed steps found in the [submitting-a-patch](submitting-a-patch.md) document to submit a patch for review.

#Advanced Level JIRA Items

As you gain experience contributing to Mesos you may want to tackle more advanced level JIRA items. These items may touch multiple components within Mesos and/or may have a significant impact on the developer or user community. In these cases, a working group of stakeholders is formed to develop a design document. The initial formation of this working group will be part of the community communication resources, e.g. the re-occurring developer sync meetings, the developer email list, the IRC channel, etc. For reference, a contributor new to an advanced level work item can refer to the work done for the [inverse offer](https://issues.apache.org/jira/browse/MESOS-1592) project.

-----

# FAQs

Q: Where can I find documentation about Mesos?
A: Mesos documentation is located in the ‘docs’ directory of the Mesos distribution. This documentation includes information about Mesos architecture and design, running Mesos, developing within the Mesos ecosystem and contributing to Mesos. Additionally, the latest documentation can be found here:
[http://mesos.apache.org/documentation/latest/index.html](http://mesos.apache.org/documentation/latest/index.html)


Q: What is a Shepherd?
A: An identified PMC/committer that works with a contributor to help shepherd a JIRA item to completion.  Shepherds should be as identified at the beginning of the design/development phase.
