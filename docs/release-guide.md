---
title: Apache Mesos - Release Guide
layout: documentation
---

# Release Guide

This guide describes the process of doing an official release of Mesos.

-----------------------------------------------------------------------

## Prerequisites

1. Ensure that you have a GPG key or generate a new one, e.g., using `gpg --gen-key`.

2. Add your GPG public key to the Apache Mesos dist repository in the KEYS file.

    - Fetch the svn repository:<br>
      `svn co https://dist.apache.org/repos/dist/release/mesos`

    - Append your public key using one of methods described in KEYS,
      e.g.,<br>
      `(gpg --list-sigs <your name> && gpg --armor --export <your name>) >> KEYS`.

    - Push the commit:<br>
      `svn ci`

3. Submit your GPG public key to a keyserver, e.g., [MIT PGP Public Key Server](https://pgp.mit.edu).

4. Add your GPG fingerprint (`gpg --fingerprint <your name>`) to your [Apache account](https://id.apache.org/).

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

    > NOTE: You should move all **Unresolved** tickets marked with `Fix Version`
      or `Target Version` as the release version to the next release version.

    > PROTIP: Use a JIRA dashboard [(example)](https://issues.apache.org/jira/secure/Dashboard.jspa?selectPageId=12326227)
      to track the progress of targeted issues as the release date approaches.

    > PROTIP: Use `bulk edit` option in JIRA to move the tickets and make sure to
      **uncheck** the option that emails everyone about the move.

2. Update and commit the `CHANGELOG` for the release. For major releases we like to call
   out any major features, API changes, or deprecations.

    > NOTE: You should use JIRA to generate the CHANGELOG for you. Click on the release
      version in [JIRA](https://issues.apache.org/jira/browse/MESOS#selectedTab=com.atlassian.jira.plugin.system.project%3Aversions-panel) and click
      on the `Release Notes`. Make sure to configure the release notes in text format.

    > NOTE: The JIRA Release Notes will list only tickets with `Fix Version` set
      to that version. You should check for any Resolved tickets that have
      `Target Version` set but not `Fix Version`. Also check for any Unresolved
      or `Duplicate`/`Invalid` tickets that incorrectly set the `Fix Version`.

3. If not already done, update and commit `configure.ac` and `CMakeLists.txt` for the release.

4. Run `support/generate-endpoint-help.py` and commit any resulting changes.

5. Update and commit `docs/configuration.md` to reflect the current state of
   the master, agent, and configure flags.

6. Update and commit `docs/upgrades.md` with instructions about how to upgrade
   a live cluster from the previous release version to this release version.

7. If this is a major release, please ensure that user documentation has been
   added for any new features.

8. Make sure that for any updates of the API, specifically the scheduler API, the public mesos protobuf definitions are part of both, `include/mesos` as well as `include/mesos/v1`. NOTE: This might actually demand code updates if any omissions were identified.

## Tagging the release candidate

1. Ensure that you can build and pass all the tests.

        $ sudo make -j<cores> distcheck

2. Run the benchmarks and compare with the previous release for any performance regressions:

        $ make bench -j<cores> GTEST_FILTER="*BENCHMARK*"

3. First tag the required SHA locally.

        $ git tag <X.Y.Z-rcR>

    > NOTE: `X.Y.Z` is based on [semantic versioning](http://semver.org/) scheme. `R` is release
            candidate version that starts with 1.

4. Tag the release externally and deploy the corresponding JAR to the [Apache maven repository](https://repository.apache.org).
   It is recommended to use the `support/tag.sh` script to accomplish this.

        $ ./support/tag.sh X.Y.Z R

    > NOTE: This script assumes that you have the requisite permissions to deploy the JAR. For
      instructions on how to set it up, please refer to `src/java/MESOS-MAVEN-README`.

    > NOTE: gnu-sed (Linux) requires `-i''` instead of the `-i ''` (space-separated) that default OSX uses.
      You may need to modify your local copy of tag.sh for it to complete successfully.

5. It is not uncommon to release multiple release candidates, with increasing release candidate
   version, if there are bugs found.

6. Update to the *next* Mesos version in `configure.ac`: change `AC_INIT([mesos], [X.Y.Z]))`, as well as in `CMakeLists.txt`: change `set(MESOS_MAJOR_VERSION X)`, `set(MESOS_MINOR_VERSION Y)`, `set(MESOS_PATCH_VERSION Z)` and then commit.

## Voting the release candidate

1. Once a release candidate is deemed worthy to be officially released you should call a vote on
   the `dev@mesos.apache.org` (and optionally `user@mesos.apache.org`) mailing list.

2. It is recommended to use the `support/vote.sh` script to vote the release candidate.

        $ ./support/vote.sh X.Y.Z R

3. The release script also spits out an email template that you could use to send the vote email.

    > NOTE: The `date -v+3d` command does not work on some platforms (e.g. Ubuntu),
      so you may need to fill in the vote end date manually. The vote should last
      for 3 business days instead of 3 calendar days anyway. Sometimes we prefer a
      weeklong vote, to allow more time for integration testing.

## Preparing a new release candidate

1. If the vote does not pass (any -1s or showstopper bugs), track the issues as new JIRAs for the release.

2. When all known issues are resolved, update the CHANGELOG with the newly fixed JIRAs.

3. Once all patches are committed to master, cherry-pick them on top of the previous release candidate tag.
   This is the same process used for point releases (e.g. 0.22.1) as well.

        $ git checkout X.Y.Z-rcR
        $ git cherry-pick abcdefgh...

4. Now go back up to the "Tagging the release candidate" section and repeat.

## Releasing the release candidate

1. You should only release an official version if the vote passes with at least **3 +1 binding votes
   and no -1 votes**. For more information, please refer to [Apache release guidelines](http://www.apache.org/dev/release.html).

2. It is recommended to use `support/release.sh` script to release the candidate.

        $ ./support/release.sh X.Y.Z R

3. The release script also spits out an email template that you could use to notify the mailing lists about
   the result of the vote and the release.

    > NOTE: Make sure you fill the email template with the names of binding voters.

## Updating the wiki

Update the wiki entry, [Mesos Release Planning](https://cwiki.apache.org/confluence/display/MESOS/Mesos+Release+Planning).

## Updating the website

1. After a successful release, add the information associated with the release in `site/data/releases.yml`. It is used to generate the release information on the website.

2. Update the [Getting Started](getting-started.md) guide to use the latest release link.

3. Check out the website from svn.

        $ svn co https://svn.apache.org/repos/asf/mesos/site mesos-site

   See our [website README](https://github.com/apache/mesos/blob/master/site/README.md/) for details on how to build the website.
   See the general [Apache project website guide](https://www.apache.org/dev/project-site.html) for details on how to publish the website.

4. Write a blog post announcing the new release and its features and major bug fixes. Include a link to the updated website.

## Remove old releases from svn

Per the guidelines [when to archive](http://www.apache.org/dev/release.html#when-to-archive), we should only keep the latest release in each version under development.

1. Checkout the mesos distribution folder: `svn co https://dist.apache.org/repos/dist/release/mesos`

2. Remove all minor versions that are no longer under development and commit the change.

## Release the version on JIRA

1. Find the released Mesos version on https://issues.apache.org/jira/plugins/servlet/project-config/MESOS/versions, and "release" it (click on "settings" --> "Release") with the correct release date.


## Update external tooling

Upload the mesos.interface package to PyPi.

  1. Create/use a PyPi account with access to the [mesos.interface submit form](https://pypi.python.org/pypi?name=mesos.interface&:action=submit_form).
     You may need to ask a current package owner to add you as an owner/maintainer.
  1. Setup your [`~/.pypirc`](https://docs.python.org/2/distutils/packageindex.html#pypirc) with your PyPi username and password.
  1. After a successful Mesos `make` (any architecture), cd to `build/src/python/interface`.
  1. Run `python setup.py register` to register this package.
  1. Run `python setup.py sdist bdist_egg upload` to upload the source distribution and egg for this package.

Update the Mesos Homebrew package.

  1. Update the [Homebrew formula for Mesos](https://github.com/Homebrew/homebrew-core/blob/master/Formula/mesos.rb) and test.
  1. Submit a PR to the [Homebrew repo](https://github.com/Homebrew/homebrew-core).
  1. Once accepted, verify that `brew install mesos` works.
