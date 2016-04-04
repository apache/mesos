---
title: Apache Mesos - Versioning
layout: documentation
---

# Mesos Versioning

The Mesos API and release versioning policy gives operators and developers clear guidelines on:

* Making modifications to the existing APIs without affecting backward compatibility.
* How long a Mesos API will be supported.
* Upgrading the Mesos installation across release versions.

API versioning was introduced in Mesos 0.24.0 and this scheme only applies to Mesos 1.0.0 and higher.

## Terminology

* **Release Versioning**: This refers to the version of Mesos that is being released. It is of the form **Mesos X.Y.Z** (X is the major version, Y is the minor version, and Z is the patch version).
* **API Versioning**: This refers to the version of the Mesos API. It is of the form **vX** (X is the major version).

## How does it work?

The Mesos APIs (constituting Scheduler, Executor, Internal, Operator/Admin APIs) will have a version in the URL. The versioned URL will have a prefix of **`/api/vN`** where "N" is the version of the API. The "/api" prefix is chosen to distinguish API resources from Web UI paths.

Examples:

* http://localhost:5050/api/v1/scheduler :  Scheduler HTTP API hosted by the master.
* http://localhost:5051/api/v1/executor  :  Executor HTTP API hosted by the agent.

A given Mesos installation might host multiple versions of the same API i.e., Scheduler API v1 and/or v2 etc.

### API version vs Release version

* To keep things simple, the stable version of the API will correspond to the major release version of Mesos.
  * For example, v1 of the API will be supported by Mesos release versions 1.0.0, 1.4.0, 1.20.0 etc.
* vN version of the API might also be supported by release versions of N-1 series but the vN API is not considered stable until the last release version of N-1 series.
 *  For example, v2 of the API might be introduced in Mesos 1.12.0 release but it is only considered stable in Mesos 1.21.0 release if it is the last release of “1” series. Note that all Mesos 1.x.y versions will still support v1 of the API.
*  The API version is only bumped if we need to make a backwards [incompatible](#api-compatibility) API change. We will strive to support a given API version for at least a year.
*  The deprecation clock for vN-1 API will start as soon as we release “N.0.0” version of Mesos. We will strive to give enough time (e.g., 6 months) for frameworks/operators to upgrade to vN API before we stop supporting vN-1 API.
*  Minor release version is bumped roughly on a monthly cycle to give a cadence of new features to users.

NOTE: Presently, for "0.X.Y" releases i.e. till we reach "1.0.0", we wait for at least 6 monthly releases before deprecating functionality. So, functionality that has been deprecated in 0.26.0 can be safely removed in 0.32.0.

### API Compatibility

The API compatibility is determined by the corresponding protobuf guarantees.

As an example, the following are considered "backwards compatible" changes for Scheduler API:

* Adding new types of Calls i.e., new types of HTTP requests to "/scheduler".
* Adding new optional fields to existing requests to "/scheduler".
* Adding new types of Events i.e., new types of chunks streamed on "/scheduler".
* Adding new header fields to chunked response streamed on "/scheduler".
* Adding new fields (or changing the order of fields) to chunks’ body streamed on "/scheduler".
* Adding new API resources (e.g., "/foobar").

The following are considered backwards incompatible changes for Scheduler API:

* Adding new required fields to existing requests to "/scheduler".
* Renaming/removing fields from existing requests to "/scheduler".
* Renaming/removing fields from chunks streamed on "/scheduler".
* Renaming/removing existing Calls.

## Upgrades

* The master and agent are typically compatible as long as they are running the same major version.
 * For example, 1.3.0 master is compatible with 1.13.0 agent.
* In rare cases, we might require the master and agent to go through a specific minor version for upgrades.
 * For example, we might require that a 1.1.0 master (and/or agent) be upgraded to 1.8.0 before it can be upgraded to 1.9.0 or later versions.

The detailed information about upgrading to a particular Mesos version would be posted [here](upgrades.md).

### Implementation Details

Most APIs in Mesos accept protobuf messages with a corresponding JSON field mapping. To support multiple versions of the API, we decoupled the versioned protobufs backing the API from the “internal” protobufs used by the Mesos code.

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
