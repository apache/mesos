---
title: Apache Mesos - Beginner Contribution Guide
layout: documentation
---

# Beginner Contributor Guide

## Introduction

The purpose of this document is to provide a first-time introduction to the process of contributing to Mesos. It focuses on submitting small documentation-only patches or trivial fixes via GitHub pull requests. If you'd like an introduction to the standard day-to-day workflow for advanced Mesos contributors, see our [advanced contribution guide](advanced-contribution.md).

## Quick Summary

To propose a small change to Mesos, simply open a PR against our public GitHub mirror at [https://github.com/apache/mesos](https://github.com/apache/mesos). Further instructions can be found below if needed.

## Download the Mesos Repository

First, download the latest development version of the Mesos codebase. In order to submit changes via GitHub pull requests, you need to fork the [Apache Mesos GitHub mirror](https://github.com/apache/mesos). Once you have your own fork, clone it to your local machine using Git.

If you're proposing a documentation-only change, then you don't need to build Mesos to get started.

If you're making a functional change to the code, then you should build Mesos first. Once you have the Mesos source code on your local machine, you can install the necessary dependencies and build it. Instructions for this process can be found in the [building](building.md) page. Note that the `./support/setup-dev.sh` script will install git hooks which will help you adhere to Mesos style when committing.

## Find a Problem to Solve

If you already have a specific issue in mind which you want to address in the codebase, that's great! Reach out to some of the [committers](committers.md) on [Mesos Slack](/community) or on the developer mailing list to discuss the improvement you'd like to make. If you want to contribute but aren't sure what to work on, you can find open issues in [JIRA](http://issues.apache.org/jira/browse/MESOS), our issue tracker. Asking on [Mesos Slack](/community) or on the developer mailing list is a great way to find out what you might work on that could have a high impact on the community.

From past experience, we have found that it's best if contributors talk to committers about their fix before writing code. This ensures that the contributor has the context necessary to make the change in a way consistent with the rest of the codebase, and helps avoid time spent on solutions which need major edits in order to get merged. Please chat with us before writing code, we'll help you design a solution!

This GitHub workflow is most appropriate for documentation-only changes and small, trivial fixes. For more significant changes, please use the [advanced contribution workflow](advanced-contribution.md).

## Make Modifications

Once you know what you're going to change, you can make your intended modifications to the codebase and then commit the changes to your local build repository. When making changes, also consider the following:

* Does documentation need to be updated to accommodate this change? (see the `docs/` folder in the repository)
* Do tests need to be added or updated? (see the `src/tests/` folder in the repository)

## Build and Test

If your changes are documentation-only, then you should view them with a markdown viewer to verify their appearance when rendered.

If you are changing any code in Mesos, then you should build and run the tests before opening a PR. You should run `make check` and ensure that all tests pass before opening a PR.

## Open a Pull Request

Once changes are completed and tested, it's time to open a Pull Request (PR) so that they can be reviewed. When your local branch is clean and ready to submit, push it to your Mesos fork. You can then open a PR against the Apache Mesos GitHub repository. Once your PR is open, you can notify the community on [Mesos Slack](/community) or on the developer mailing list.

# Getting Started Guidance

The following links provide additional guidance as you get started contributing to Apache Mesos.

## JIRA

[JIRA](http://issues.apache.org/jira/browse/MESOS) is the issue tracking system for all bug fixes and feature additions in Mesos. When contributing to Mesos, all assignments, discussions, and resolution proposals for a specific issue should be documented as comments within the JIRA item. If you contribute something which is more than a trivial fix, it should probably be tracked in JIRA.

## Identifying a JIRA Issue

If you'd like to find an existing issue to work on yourself, identify a Mesos JIRA ticket that is currently unassigned. It is highly recommended to start contributing to beginner-level issues and move to more advanced issues over time. The JIRA issue-tracking system can be filtered based on labels. The following labels can be used to identify beginner-level JIRA tickets:

* newbie
* newbie++
* beginner
* beginners

If you'd like to work on existing issues in Mesos, this will likely require the submission of patches large enough that you should use the [advanced contribution workflow](advanced-contribution.md).
