---
title: Apache Mesos - Release and Support Policy
layout: documentation
---

# Mesos Release and Support policy

The Mesos versioning and release policy gives operators and developers clear guidelines on:

* Making modifications to the existing APIs without affecting backward compatibility.
* How long a Mesos API will be supported.
* Upgrading a Mesos installation across release versions.

This document describes the release strategy for Mesos post 1.0.0 release.


## Release Schedule

Mesos releases are time-based, though we do make limited adjustments to the release schedule to accommodate feature development. This gives users and developers a predictable cadence to consume and produce features, while ensuring that each release can include the developments that users are waiting for.

If a feature is not ready by the time a release is cut, that feature should be disabled. This means that features should be developed in such a way that they are opt-in by default and can be easily disabled (e.g., flag).

A new Mesos release is cut approximately every **3 months**. The versioning scheme is [SemVer](http://semver.org). Typically, the minor release version is incremented by 1 (e.g., 1.1, 1.2, 1.3 etc) for every release, unless it is a major release.

Every (minor) release is a stable release and recommended for production use. This means a release candidate will go through rigorous testing (unit tests, integration tests, benchmark tests, cluster tests, scalability, etc.) before being officially released. In the rare case that a regular release is not deemed stable, a patch release will be released that will stabilize it.

At any given time, 3 releases are supported: the latest release and the two prior. Support means fixing of *critical issues* that affect the release. Once an issue is deemed critical, it will be fixed in only those **affected** releases that are still **supported**. This is called a patch release and increments the patch version by 1 (e.g., 1.2.1). Once a release reaches End Of Life (i.e., support period has ended), no more patch releases will be made for that release. Note that this is not related to backwards compatibility guarantees and deprecation periods (discussed later).

Which issues are considered critical?

* Security fixes
* Compatibility regressions
* Functional regressions
* Performance regressions
* Fixes for 3rd party integration (e.g., Docker remote API)

Whether an issue is considered critical or not is sometimes subjective. In some cases it is obvious and sometimes it is fuzzy. Users should work with committers to figure out the criticality of an issue and get agreement and commitment for support.

Patch releases are normally done **once per month**.

If a particular issue is affecting a user and the user cannot wait until the next scheduled patch release, they can request an off-schedule patch release for a specific supported version. This should be done by sending an email to the dev list.

## Upgrades

All stable releases will be loosely compatible. Loose compatibility means:

* Master or agent can be upgraded to a new release version as long as they or the ecosystem components (scheduler, executor, zookeeper, service discovery layer, monitoring etc) do not depend on deprecated features (e.g., deprecated flags, deprecated metrics).
* There should be no unexpected effect on externally visible behavior that is not deprecated. See API compatibility section for what should be expected for Mesos APIs.

> NOTE: The compatibility guarantees do not apply to modules yet. See Modules section below for details.


This means users should be able to upgrade (as long as they are not depending on deprecated / removed features) Mesos master or agent from a stable release version N directly to another stable release version M without having to go through intermediate release versions. For the purposes of upgrades, a stable release means the release with the latest patch version. For example, among 1.2.0, 1.2.1, 1.3.0, 1.4.0, 1.4.1 releases 1.2.1, 1.3.0 and 1.4.1 are considered stable and so a user should be able to upgrade from 1.2.1 directly to 1.4.1. Look at the API compatability section below for how frameworks can do seamless upgrades.

The deprecation period for any given feature will be **6 months**. Having a set period allows Mesos developers to not indefinitely accrue technical debt and allows users time to plan for upgrades.

The detailed information about upgrading to a particular Mesos version would be posted [here](upgrades.md).


## API versioning

The Mesos APIs (constituting Scheduler, Executor, Internal, Operator/Admin APIs) will have a version in the URL. The versioned URL will have a prefix of **`/api/vN`** where "N" is the version of the API. The "/api" prefix is chosen to distinguish API resources from Web UI paths.

Examples:

* http://localhost:5050/api/v1/scheduler :  Scheduler HTTP API hosted by the master.
* http://localhost:5051/api/v1/executor  :  Executor HTTP API hosted by the agent.

A given Mesos installation might host multiple versions of the same API i.e., Scheduler API v1 and/or v2 etc.

### API version vs Release version

* To keep things simple, the stable version of the API will correspond to the major release version of Mesos.
  * For example, v1 of the API will be supported by Mesos release versions 1.0.0, 1.4.0, 1.20.0 etc.
* vN version of the API might also be supported by release versions of N-1 series but the vN API is not considered stable until the last release version of N-1 series.
 *  For example, v2 of the API might be introduced in Mesos 1.12.0 release but it is only considered stable in Mesos 1.21.0 release if it is the last release of "1" series. Note that all Mesos 1.x.y versions will still support v1 of the API.
*  The API version is only bumped if we need to make a backwards [incompatible](#api-compatibility) API change. We will strive to support a given API version for at least a year.
*  The deprecation clock for vN-1 API will start as soon as we release "N.0.0" version of Mesos. We will strive to give enough time (e.g., 6 months) for frameworks/operators to upgrade to vN API before we stop supporting vN-1 API.

<a name="api-compatibility"></a>
### API Compatibility

The API compatibility is determined by the corresponding protobuf guarantees.

As an example, the following are considered "backwards compatible" changes for Scheduler API:

* Adding new types of Calls i.e., new types of HTTP requests to "/scheduler".
* Adding new optional fields to existing requests to "/scheduler".
* Adding new types of Events i.e., new types of chunks streamed on "/scheduler".
* Adding new header fields to chunked response streamed on "/scheduler".
* Adding new fields (or changing the order of fields) to chunks' body streamed on "/scheduler".
* Adding new API resources (e.g., "/foobar").

The following are considered backwards incompatible changes for Scheduler API:

* Adding new required fields to existing requests to "/scheduler".
* Renaming/removing fields from existing requests to "/scheduler".
* Renaming/removing fields from chunks streamed on "/scheduler".
* Renaming/removing existing Calls.


## Implementation Details

### Release branches

For regular releases, the work is done on the master branch. There are no feature branches but there will be release branches.

When it is time to cut a minor release, a new branch (e.g., 1.2.x) is created off the master branch. We chose 'x' instead of patch release number to disambiguate branch names from tag names. Then the first RC (-rc1) is tagged on the release branch. Subsequent RCs, in case the previous RCs fail testing, should be tagged on the release branch.

Patch releases are also based off the release branches. Typically the fix for an issue that is affecting supported releases lands on the master branch and is then backported to the release branch(es). In rare cases, the fix might directly go into a release branch without landing on master (e.g.,  fix / issue is not applicable to master).

Having a branch for each minor release reduces the amount of work a release manager needs to do when it is time to do a release. It is the responsibility of the committer of a fix to commit it to all the affecting release branches. This is important because the committer has more context about the issue / fix at the time of the commit than a release manager at the time of release. The release manager of a minor release will be responsible for all its patch releases as well. Just like the master branch, history rewrites are not allowed in the release branch (i.e., no git push --force).


### API protobufs


Most APIs in Mesos accept protobuf messages with a corresponding JSON field mapping. To support multiple versions of the API, we decoupled the versioned protobufs backing the API from the "internal" protobufs used by the Mesos code.

For example, the protobufs for the v1 Scheduler API are located at:

```
include/mesos/v1/scheduler/scheduler.proto

package mesos.v1.scheduler;
option java_package = "org.apache.mesos.v1.scheduler";
option java_outer_classname = "Protos";
...
```

The corresponding internal protobufs for the Scheduler API are located at:

```
include/mesos/scheduler/scheduler.proto

package mesos.scheduler;
option java_package = "org.apache.mesos.scheduler";
option java_outer_classname = "Protos";
...
```

The users of the API send requests (and receive responses) based on the versioned protobufs. We implemented [evolve](https://github.com/apache/mesos/blob/master/src/internal/evolve.hpp)/[devolve](https://github.com/apache/mesos/blob/master/src/internal/devolve.hpp) converters that can convert protobufs from any supported version to the internal protobuf and vice versa.

Internally, message passing between various Mesos components would use the internal unversioned protobufs. When sending response (if any) back to the user of the API, the unversioned protobuf would be converted back to a versioned protobuf.
