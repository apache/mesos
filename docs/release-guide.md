---
layout: documentation
---

# Release Guide

This guide describes the process of doing an official release of Mesos.

-----------------------------------------------------------------------

## Prerequisites

1. Ensure that you have a GPG key or generate a new one, e.g., using `gpg --gen-key`.

2. Add your GPG public key to the Apache Mesos dist repository in the KEYS file.
  - Fetch the svn repository `svn co https://dist.apache.org/repos/dist/release/mesos`
  - Append your public key using one of methods described in KEYS,
    e.g., `(gpg --list-sigs <your name> && gpg --armor --export <your name>) >> KEYS`.
  - Push the commit: `svn ci`

3. Submit your GPG public key to a keyserver, e.g., [MIT PGP Public Key Server](https://pgp.mit.edu).

4. Add your GPG fingerprint to your [Apache account](https://id.apache.org/).

5. Create a Maven settings file (`~/.m2/settings.xml`) for the Apache
   servers where you must copy your encrypted Apache password which
   you can get from running `mvn --encrypt-password` (NOTE: you may
   need to first generate a [master
   password](http://maven.apache.org/guides/mini/guide-encryption.html).

        <settings>
          <servers>
            <server>
              <id>apache.snapshots.https</id>
              <username>APACHE USERNAME</username>
              <password>APACHE ENCRYPTED PASSWORD</password>
            </server>
            <server>
              <id>apache.releases.https</id>
              <username>APACHE USERNAME</username>
              <password>APACHE ENCRYPTED PASSWORD</password>
            </server>
          </servers>
        </settings>

6. Use `gpg-agent` to avoid typing your passphrase repeatedly.


## Preparation

1. Go to [Apache Jira](https://issues.apache.org/jira/browse/MESOS) and make sure that
   the CHANGELOG for the release version is up to date.

    > NOTE: You should move all **Unresolved** tickets marked with `Fix Version` as the
            release version to the next release version.

    > PROTIP: Use `bulk edit` option in JIRA to move the tickets and make sure to
              **uncheck** the option that emails everyone about the move.

2. Update and commit the `CHANGELOG` for the release.

    > NOTE: You should use JIRA to generate the CHANGELOG for you. Click on the release
     version in [JIRA](https://issues.apache.org/jira/browse/MESOS#selectedTab=com.atlassian.jira.plugin.system.project%3Aversions-panel) and click
     on the `Release Notes`. Make sure to configure the release notes in text format.

3. If not already done, update and commit `configure.ac` for the release.

4. Update and commit `docs/configuration.md` to reflect the current state of
   the master, slave, and configure flags.

5. Update and commit `docs/upgrades.md` with instructions about how to upgrade
   a live cluster from the previous release version to this release version.

6. If this is a major release please write and commit documentation for this feature.


## Tagging the release candidate

1. Ensure that you can build and pass all the tests.

        $ make -j3 distcheck

2. First tag the required SHA locally.

        $ git tag <X.Y.Z-rcR>

    > NOTE: `X.Y.Z` is based on [semantic versioning](http://semver.org/) scheme. `R` is release
            candidate version that starts with 1.

3. Tag the release externally and deploy the corresponding JAR to the Apache maven repository.
   It is recommended to use the `support/tag.sh` script to accomplish this.

        $ ./support/tag.sh X.Y.Z R

    > NOTE: This script assumes that you have the requisite permissions to deploy the JAR. For
      instructions on how to set it up, please refer to `src/java/MESOS-MAVEN-README`.

4. It is not uncommon to release multiple release candidates, with increasing release candidate
   version, if there are bugs found.

5. Update to the *next* Mesos version in `configure.ac`: change `AC_INIT([mesos], [X.Y.Z]))` and commit.

## Voting the release candidate

1. Once a release candidate is deemed worthy to be officially released you should call a vote on
   the `dev@mesos.apache.org` (and optionally `user@mesos.apache.org`) mailing list.

2. It is recommended to use the `support/vote.sh` script to vote the release candidate.

        $ ./support/vote.sh X.Y.Z R

3. The release script also spits out an email template that you could use to send the vote email.


## Releasing the release candidate

1. You should only release an official version if the vote passes with at least **3 +1 binding votes
   and no -1 votes**. For more information, please refer to [Apache release guidelines](http://www.apache.org/dev/release.html).

2. It is recommended to use `support/release.sh` script to release the candidate.

        $ ./support/release.sh X.Y.Z R

3. The release script also spits out an email template that you could use to notify the mailing lists about
   the result of the vote and the release.

    > NOTE: Make sure you fill the email template with the names of binding voters.


## Updating the website

1. After a successful release please update the website pointing to the new release.

2. It is also recommended to write a blog post announcing the feature.

## Remove old releases from svn

Per the guidelines [when to archive](http://www.apache.org/dev/release.html#when-to-archive), we should only keep the latest release in each version under development.

1. Checkout the mesos distribution folder: `svn co https://dist.apache.org/repos/dist/release/mesos`

2. Remove all minor versions that are no longer under development and commit the change.

## Set the release date

1. Find the released Mesos version on https://issues.apache.org/jira/plugins/servlet/project-config/MESOS/versions, and update the release date.

## Update external tooling

1. Update the Mesos Homebrew package.
  1. Update the [Homebrew formula for Mesos](https://github.com/Homebrew/homebrew/blob/master/Library/Formula/mesos.rb) and test.
  1. Submit a PR to the [Homebrew repo](https://github.com/Homebrew/homebrew).
  1. Once accepted, verify that `brew install mesos` works.
