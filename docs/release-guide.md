---
title: Apache Mesos - Release Guide
layout: documentation
---

# Release Guide

This guide describes the process of doing an official release of Mesos.


## Prerequisites

1. Ensure that you have a GPG key or generate a new one, e.g., using `gpg --gen-key`.

2. Add your GPG public key to the Apache Mesos dist repository in the KEYS file.

   * Fetch the svn repository:

          $ svn co https://dist.apache.org/repos/dist/release/mesos

   * Append your public key using one of methods described in KEYS, e.g.:

          $ (gpg --list-sigs <your name> && gpg --armor --export <your name>) >> KEYS

   * Push the commit:

          $ svn ci

3. Submit your GPG public key to a keyserver, e.g.,
   [MIT PGP Public Key Server](https://pgp.mit.edu).

4. Add your GPG fingerprint (`gpg --fingerprint <your name>`) to your
   [Apache account](https://id.apache.org/).

5. Create a Maven settings file (`~/.m2/settings.xml`) for the Apache
   servers where you must copy your encrypted Apache password which
   you can get from running `mvn --encrypt-password`.

   **NOTE:** You may need to first generate a
   [master password](http://maven.apache.org/guides/mini/guide-encryption.html).

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

6. Use `gpg-agent` to avoid typing your passphrase repeatedly:

        $ export GPG_TTY="$(tty)" && eval $(gpg-agent --daemon)


## Preparation

1. If this is a regular release, create a new release branch (<major>.<minor>.x)
   based off the current master.

        $ git checkout origin/master -b X.Y.x

   Now, update master branch to the *next*  minor version in `configure.ac`:
   change `AC_INIT([mesos], [X.Y.Z]))`, as well as in `CMakeLists.txt`:
   change `set(MESOS_MAJOR_VERSION X)`, `set(MESOS_MINOR_VERSION Y)`,
   `set(MESOS_PATCH_VERSION Z)` and then commit.

   If this is a patch release, use the existing release branch.

2. Go to [Apache JIRA](https://issues.apache.org/jira/browse/MESOS) and make
   sure that the `CHANGELOG` for the release version is up to date.

   **NOTE:** For all **Unresolved** tickets marked with `Fix Version` or
   `Target Version` as the release version, try to negotiate with the
   ticket owner or shepherd if the release should wait for the ticket to
   be resolved or if the `Fix Version` should be bumped to the next version.

   **PROTIP:** Use a JIRA dashboard
   [(example)](https://issues.apache.org/jira/secure/Dashboard.jspa?selectPageId=12329720)
   to track the progress of targeted issues as the release date approaches. This
   JIRA filter may be useful (`<X.Y.Z>` is the release version):
   `project = MESOS AND "Target Version/s" = <X.Y.Z> AND (fixVersion != <X.Y.Z> OR fixVersion = EMPTY)`
   Note, you may need to request permission to create shared dashboard and filters by opening
   an Apache INFRA ticket.

   **PROTIP:** Use `bulk edit` option in JIRA to move the tickets and make sure
   to **uncheck** the option that emails everyone about the move to avoid
   spamming.

3. Update and commit the `CHANGELOG` for the release, on both the **master**
   and the release branch.

   For regular releases:

   * Make sure all new API changes, deprecations, major features, and features
     graduating from experimental to stable are called out at the top of the
     `CHANGELOG`. This JIRA query may be helpful in identifying some of these
     (`<X.Y.Z>` is the release version):
     `project = MESOS AND "Target Version/s" = <X.Y.Z> AND type = Epic`

   * Ensure that the "Unresolved Critical Issues" section is populated
     correctly. This JIRA query may be helpful:
     `project = Mesos AND type = bug AND status != Resolved AND priority IN (blocker, critical)`

   * Prepare a full list of experimental features. The easiest way to do this
     is to take the list from the previous minor release, remove features that
     have been declared stable, and those, that declared experimental in the
     current release. Do not forget to mention features transitioning from
     experimental to stable in this release at the top of the `CHANGELOG`.

   **NOTE:** You should use JIRA to generate the major portion of the
   `CHANGELOG` for you. Click on the release version in
   [JIRA](https://issues.apache.org/jira/browse/MESOS#selectedTab=com.atlassian.jira.plugin.system.project%3Aversions-panel)
   and click on the `Release Notes`. Make sure to configure the release notes
   in text format.

   **NOTE:** The JIRA Release Notes will list only tickets with `Fix Version`
   set to that version. You should check for any Resolved tickets that have
   `Target Version` set but not `Fix Version`. Also check for any Unresolved
   or `Duplicate`/`Invalid` tickets that incorrectly set the `Fix Version`.

   Update the `CHANGELOG` on master branch and then cherry pick onto the release
   branch to ensure both versions stay in sync.

4. Ensure version in `configure.ac` and `CMakeLists.txt` is correctly set for
   the release. Do not forget to remove "(WIP)" suffix from the release notes'
   title.

5. Update and commit `docs/configuration.md` to reflect the current state of
   the master, agent, and configure flags. Update it on master branch and then
   cherry pick onto the release branch.

6. If this is a regular release, update and commit `docs/upgrades.md` with
   instructions about how to upgrade a live cluster from the previous release
   version to this release version. Update it on master branch and then cherry
   pick onto the release branch.

7. If this is a regular release, please ensure that user documentation has been
   added for any new features.

8. Make sure that for any updates of the API, specifically the scheduler API,
   the public mesos protobuf definitions are part of both, `include/mesos` as
   well as `include/mesos/v1`.

   **NOTE:** This might actually demand code updates if any omissions were
   identified.

9. Push your changes on master branch and the new release branch if this is a
   regular release, push your changes on the existing release branch if this is
   a patch release.


## Tagging and Voting the Release Candidate

1. Ensure that you can build and pass all the tests.

        $ sudo make -j<cores> distcheck

2. Run the benchmarks and compare with the previous release for any performance
   regressions:

        $ make bench -j<cores> GTEST_FILTER="*BENCHMARK*"

3. First tag the required SHA locally.

        $ git tag -a <X.Y.Z-rcR> -m "Tagging Mesos <X.Y.X-rcR>."

   **NOTE:** `X.Y.Z` is based on [semantic versioning](http://semver.org/)
   scheme. `R` is release candidate version that starts with 1.

4. Tag the release externally and deploy the corresponding JAR to the
   [Apache Maven repository](https://repository.apache.org). It is recommended
   to use the `support/vote.sh` script to accomplish this.

        $ ./support/vote.sh X.Y.Z R

   **NOTE:** This script assumes that you have the requisite permissions to
   deploy the JAR. For instructions on how to set it up, please refer to
   `src/java/MESOS-MAVEN-README`.

5. The script also spits out an email template that you could use to
   send the vote email.

   **NOTE:** The `date -v+3d` command does not work on some platforms (e.g.
   Ubuntu), so you may need to fill in the vote end date manually. The vote
   should last for 3 business days instead of 3 calendar days anyway. Sometimes
   we allow a longer vote, to allow more time for integration testing.


## Preparing a New Release Candidate

1. If the vote does not pass (any -1s or showstopper bugs)
   1. Send a reply to the original VOTE email with subject "[RESULT][VOTE] Release Apache Mesos X.Y.Z (rcN)" and mention that the vote is canceled.
   2. Go to [Apache Maven staging repositories](https://repository.apache.org/#stagingRepositories) and "Drop" the staging repository containing the JAR (you can find the exact link to the staging repository in the original VOTE email).
   3. Track the issues as new JIRAs for the release.

2. When all known issues are resolved, update the `CHANGELOG` with the newly
   fixed JIRAs.

3. Once all patches are committed to master, cherry-pick them on to the
   corresponding release branch. This is the same process used for patch
   releases (e.g., 1.0.2) as well.

        $ git checkout X.Y.x
        $ git cherry-pick abcdefgh...

4. Now go back up to the "Tagging and Voting the Release Candidate" section and repeat.


## Releasing the Release Candidate

1. You should only release an official version if the vote passes with at
   least **3 +1 binding votes and no -1 votes**. For more information, please
   refer to [Apache release guidelines](http://www.apache.org/dev/release.html).

2. It is recommended to use `support/release.sh` script to release the candidate.

        $ ./support/release.sh X.Y.Z R

3. The release script also spits out an email template that you could use to
   notify the mailing lists about the result of the vote and the release.

   **NOTE:** Make sure you fill the email template with the names of binding
   and non-binding voters.

4. Update the version in `configure.ac` and `CMakeLists.txt` in the **release**
   branch to the **next** patch version.


## Updating the Website

1. After a successful release, add the information associated with the release
   in `site/data/releases.yml`. It is used to generate the release information
   on the website.

2. Update the [Building](building.md) guide to use the latest release link.

3. See our [website README](https://github.com/apache/mesos/blob/master/site/README.md)
   for details on how to build/preview the website locally, as well as information on
   how Mesos-Websitebot automatically publishes the website when changes are detected.

4. Write a blog post announcing the new release and its features and major bug
   fixes. Include a link to the updated website.

   * This command may be helpful to gather the list of all contributors between
     two tags:
     `git log --pretty=format:%an <tagX>..<tagY> | sort | uniq | awk '{print}' ORS=', '`

   * Mention the blog post in `site/data/releases.yml`.

5. Post a tweet from the https://twitter.com/apachemesos account, please contact
   the PMC if you need the account password (or want someone to post the tweet on
   your behalf).


## Removing Old Releases from svn

Per the guidelines [when to archive](http://www.apache.org/dev/release.html#when-to-archive),
we should only keep the latest release in each version under development.

1. Checkout the mesos distribution folder:

        $ svn co https://dist.apache.org/repos/dist/release/mesos

2. Remove all minor versions that are no longer under development and commit
   the change.


## Releasing the Version on JIRA

Find the released Mesos version
[here](https://issues.apache.org/jira/plugins/servlet/project-config/MESOS/versions),
and "release" it with the correct release date by clicking on
"settings"&nbsp;&rarr;&nbsp;"Release". Also, make sure to add the names of the
release managers in "Description" section.


## Updating External Tooling

Upload the mesos.interface package to PyPi.

1. Create/use a PyPi account with access to the
   [mesos.interface submit form](https://pypi.python.org/pypi?name=mesos.interface&:action=submit_form).
   You may need to ask a current package owner to add you as an owner/maintainer.

2. Setup your [`~/.pypirc`](https://docs.python.org/2/distutils/packageindex.html#pypirc)
   with your PyPi username and password.

3. After a successful Mesos `make` (any architecture), cd to
   `build/src/python/interface`.

4. Run `python setup.py register` to register this package.

5. Run `python setup.py sdist bdist_egg upload` to upload the source
   distribution and egg for this package.

Update the Mesos Homebrew package.

1. Update the [Homebrew formula for Mesos](https://github.com/Homebrew/homebrew-core/blob/master/Formula/mesos.rb)
   and test.

2. Submit a PR to the [Homebrew repo](https://github.com/Homebrew/homebrew-core).

3. Once accepted, verify that `brew install mesos` works.

Update Wikipedia:

1. Update the [Wikipedia article](https://en.wikipedia.org/wiki/Apache_Mesos) to mention the
   latest stable release in the info box.

Update Reddit: (optional)

1. Add a post for the Release to the [Mesos Reddit](https://www.reddit.com/r/mesos/).
