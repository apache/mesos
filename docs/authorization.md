---
title: Apache Mesos - Authorization
layout: documentation
---

# Authorization

In Mesos, the authorization subsystem allows the operator to configure the
actions that certain principals are allowed to perform. For example, the
operator can use authorization to ensure that principal `foo` can only register
frameworks in role `bar`, and no other principals can register frameworks in
any role.

A reference implementation _local authorizer_ provides basic security for most
use cases. This authorizer is configured using Access Control Lists (ACLs).
Alternative implementations could express their authorization rules in
different ways. The local authorizer is used if the
[`--authorizers`](configuration.md) flag is not specified (or manually set to
the default value `local`) and ACLs are specified via the
[`--acls`](configuration.md) flag.

This document is divided into two main sections. The first section explores the
concepts necessary to successfully configure the local authorizer. The second
briefly discusses how to implement a custom authorizer; this section is not
directed at operators but at engineers who wish to build their own authorizer
back end.

## Local Authorizer

### Role vs. Principal

A principal identifies an entity (i.e., a framework or an operator) that
interacts with Mesos. A role, on the other hand, is used to associate resources
with frameworks in various ways. A useful analogy can be made with user
management in the Unix world: principals correspond to usernames, while roles
approximately correspond to groups. For more information about roles, see the
[roles documentation](roles.md).

In a real-world organization, principals and roles might be used to represent
various individuals or groups; for example, principals could correspond to
people responsible for particular frameworks, while roles could correspond to
departments within the organization which run frameworks on the cluster. To
illustrate this point, consider a company that wants to allocate datacenter
resources amongst multiple departments, one of which is the accounting
department. Here is a possible scenario in which the accounting department
launches a Mesos framework and then attempts to destroy a persistent volume:

* An accountant launches their framework, which authenticates with the Mesos
  master using its `principal` and `secret`. Here, let the framework principal
  be `payroll-framework`; this principal represents the trusted identity of the
  framework.
* The framework now sends a registration message to the master. This message
  includes a `FrameworkInfo` object containing a `principal` and a `role`; in
  this case, it will use the role `accounting`. The principal in this message
  must be `payroll-framework`, to match the one used by the framework for
  authentication.
* The master consults the local authorizer, which in turn looks through its ACLs
  to see if it has a `RegisterFramework` ACL which authorizes the principal
  `payroll-framework` to register with the `accounting` role. It does find such
  an ACL, the framework registers successfully. Now that the framework belongs
  to the `accounting` role, any [weights](roles.md),
  [reservations](reservation.md), [persistent volumes](persistent-volume.md),
  or [quota](quota.md) associated with the accounting department's role will
  apply. This allows operators to control the resource consumption of this
  department.
* Suppose the framework has created a persistent volume on an agent which it
  now wishes to destroy. The framework sends an `ACCEPT` call containing an
  offer operation which will `DESTROY` the persistent volume.
* However, datacenter operators have decided that they don't want the accounting
  frameworks to delete volumes. Rather, the operators will manually remove the
  accounting department's persistent volumes to ensure that no important
  financial data is deleted accidentally. To accomplish this, they have set a
  `DestroyVolume` ACL which asserts that the principal `payroll-framework` can
  destroy volumes created by a `creator_principal` of `NONE`; in other words,
  this framework cannot destroy persistent volumes, so the operation will be
  refused.

### ACLs

When authorizing an action, the local authorizer proceeds through a list of
relevant rules until it finds one that can either grant or deny permission to
the subject making the request. These rules are configured with Access Control
Lists (ACLs) in the case of the local authorizer. The ACLs are defined with a
JSON-based language via the [`--acls`](configuration.md) flag.

Each ACL consist of an array of JSON objects. Each of these objects has two
entries. The first, `principals`, is common to all actions and describes the
subjects which wish to perform the given action. The second entry varies among
actions and describes the object on which the action will be executed. Both
entries are specified with the same type of JSON object, known as `Entity`. The
local authorizer works by comparing `Entity` objects, so understanding them is
key to writing good ACLs.

An `Entity` is essentially a container which can either hold a particular value
or specify the special types `ANY` or `NONE`.

A global field which affects all ACLs can be set. This field is called
`permissive` and it defines the behavior when no ACL applies to the request
made. If set to `true` it will allow by default all non matching requests, if
set to `false` it will reject all non matching requests.

More information about the structure of the ACLs can be found in
[their definition](https://github.com/apache/mesos/blob/master/include/mesos/authorizer/acls.proto)
inside the Mesos source code.

Note that ACLs are compared in the order that they are specified. In other
words, if an ACL allows some action and a later ACL forbids it, the action is
allowed; likewise, if the ACL forbidding the action appears earlier than the
one allowing the action, the action is forbidden. If no ACLs match a request,
the request is authorized if the ACLs are permissive (which is the default
behavior). If `permissive` is explicitly set to false, all non-matching requests
are declined.

### Authorizable Actions

Currently the local authorizer configuration format supports the following
entries, each representing an authorizable action:

|Action Name|Subject|Object|Description|
|-----------|-------|------|-----------|
|`register_frameworks`|Framework principal.|Resource [roles](roles.md) of the framework.|(Re-)registering of frameworks.|
|`run_tasks`|Framework principal.|UNIX user to launch the task as.|Launching tasks/executors by a framework.|
|`teardown_frameworks`|Operator username.|Principals whose frameworks can be shutdown by the operator.|Tearing down frameworks.|
|`reserve_resources`|Framework principal or Operator username.|Resource role of the reservation.|[Reserving](reservation.md) resources.|
|`unreserve_resources`|Framework principal or Operator username.|Principals whose resources can be unreserved by the operator.|[Unreserving](reservation.md) resources.|
|`create_volumes`|Framework principal or Operator username.|Resource role of the volume.|Creating [volumes](persistent-volumes.md).|
|`destroy_volumes`|Framework principal or Operator username.|Principals whose volumes can be destroyed by the operator.|Destroying [volumes](persistent-volumes.md).|
|`get_quotas`|Operator username.|Resource role whose quota status will be queried.|Querying [quota](quota.md) status for roles.|
|`update_quotas`|Operator username.|Resource role whose quota will be updated.|Modifying [quotas](quota.md) for roles.|
|`update_weights`|Operator username.|Resource roles whose weights can be updated by the operator.|Updating weights.|
|`view_framework`|UNIX user of whom executors can be viewed.|`Framework_Info` which can be viewed.|Filtering http endpoints.|
|`view_executor`|UNIX user of whom executors can be viewed.|`Executor_Info` and `Framework_Info` which can be viewed.|Filtering http endpoints.|
|`view_task`|UNIX user of whom tasks can be viewed.|(`Task` or `Task_Info`) and `Framework_Info` which can be viewed.|Filtering http endpoints.|
|`access_sandboxes`|Operator username.|Operating system user whose executor/task sandboxes can be accessed.|Access task sandboxes.|
|`access_mesos_logs`|Operator username.|Implicitly given. A user should only use types ANY and NONE to allow/deny access to the log.|Access Mesos logs.|

### Examples

Consider for example the following ACL: Only principal `foo` can register
frameworks within the `analytics` role. All principals can register to any
other role (including the principal `foo` since permissive is the default
behavior).

```json
{
  "register_frameworks": [
                           {
                             "principals": {
                               "values": ["foo"]
                             },
                             "roles": {
                               "values": ["analytics"]
                             }
                           },
                           {
                             "principals": {
                               "type": "NONE"
                             },
                             "roles": {
                               "values": ["analytics"]
                             }
                           }
                         ]
}
```

Principal `foo` can register frameworks with the `analytics` and `ads` roles
and no other role. Any other principal (or framework without a principal) can
register frameworks with any role.

```json
{
  "register_frameworks": [
                           {
                             "principals": {
                               "values": ["foo"]
                             },
                             "roles": {
                               "values": ["analytics", "ads"]
                             }
                           },
                           {
                             "principals": {
                               "values": ["foo"]
                             },
                             "roles": {
                               "type": "NONE"
                             }
                           }
                         ]
}
```

Only principal `foo` and no one else can register frameworks with the
`analytics` role. Any other principal (or framework without a principal) can
register frameworks with any other role.

```json
{
  "register_frameworks": [
                           {
                             "principals": {
                               "values": ["foo"]
                             },
                             "roles": {
                               "values": ["analytics"]
                             }
                           },
                           {
                             "principals": {
                               "type": "NONE"
                             },
                             "roles": {
                               "values": ["analytics"]
                             }
                           }
                         ]
}
```

Principal `foo` can register frameworks with the `analytics` role and no other
role. No other principal can register frameworks with any role, including `*`.

```json
{
  "permissive": false,
  "register_frameworks": [
                           {
                             "principals": {
                               "values": ["foo"]
                             },
                             "roles": {
                               "values": ["analytics"]
                             }
                           }
                         ]
}
```

In the following example `permissive` is set to `false`; hence, principals can
only run tasks as operating system users `guest` or `bar`, but not as any other
user.

```json
{
  "permissive": false,
  "run_tasks": [
                 {
                   "principals": { "type": "ANY" },
                   "users": { "values": ["guest", "bar"] }
                 }
               ]
}
```

Principals `foo` and `bar` can run tasks as the agent operating system user
`alice` and no other user. No other principal can run tasks.

```json
{
  "permissive": false,
  "run_tasks": [
                 {
                   "principals": { "values": ["foo", "bar"] },
                   "users": { "values": ["alice"] }
                 }
               ]
}
```

Principal `foo` can run tasks only as the agent operating system user `guest`
and no other user. Any other principal (or framework without a principal) can
run tasks as any user.

```json
{
  "run_tasks": [
                 {
                   "principals": { "values": ["foo"] },
                   "users": { "values": ["guest"] }
                 },
                 {
                   "principals": { "values": ["foo"] },
                   "users": { "type": "NONE" }
                 }
               ]
}
```

No principal can run tasks as the agent operating system user `root`. Any
principal (or framework without a principal) can run tasks as any other user.

```json
{
  "run_tasks": [
                 {
                   "principals": { "type": "NONE" },
                   "users": { "values": ["root"] }
                 }
               ]
}
```

The order in which the rules are defined is important. In the following
example, the ACLs effectively forbid anyone from tearing down frameworks even
though the intention clearly is to allow only `admin` to shut them down:

```json
{
  "permissive": false,
  "teardown_frameworks": [
                           {
                             "principals": { "type": "NONE" },
                             "framework_principals": { "type": "ANY" }
                           },
                           {
                             "principals": { "type": "admin" },
                             "framework_principals": { "type": "ANY" }
                           }
                         ]
}
```

The previous ACL can be fixed as follows:

```json
{
  "permissive": false,
  "teardown_frameworks": [
                           {
                             "principals": { "type": "admin" },
                             "framework_principals": { "type": "ANY" }
                           },
                           {
                             "principals": { "type": "NONE" },
                             "framework_principals": { "type": "ANY" }
                           }
                         ]
}
```

The `ops` principal can teardown any framework using the "/teardown" HTTP
endpoint. No other principal can teardown any frameworks.

```json
{
  "permissive": false,
  "teardown_frameworks": [
                           {
                             "principals": {
                               "values": ["ops"]
                             },
                             "framework_principals": {
                               "type": "ANY"
                             }
                           }
                         ]
}
```

The principal `foo` can reserve resources for any role, and no other principal
can reserve resources.

```json
{
  "permissive": false,
  "reserve_resources": [
                         {
                           "principals": {
                             "values": ["foo"]
                           },
                           "roles": {
                             "type": "ANY"
                           }
                         }
                       ]
}
```

The principal `foo` cannot reserve resources, and any other principal (or
framework without a principal) can reserve resources for any role.

```json
{
  "reserve_resources": [
                         {
                           "principals": {
                             "values": ["foo"]
                           },
                           "roles": {
                             "type": "NONE"
                           }
                         }
                       ]
}
```

The principal `foo` can reserve resources only for roles `prod` and `dev`, and
no other principal (or framework without a principal) can reserve resources for
any role.

```json
{
  "permissive": false,
  "reserve_resources": [
                         {
                           "principals": {
                             "values": ["foo"]
                           },
                           "roles": {
                             "values": ["prod", "dev"]
                           }
                         }
                       ]
}
```

The principal `foo` can unreserve resources reserved by itself and by the
principal `bar`. The principal `bar`, however, can only unreserve its own
resources. No other principal can unreserve resources.

```json
{
  "permissive": false,
  "unreserve_resources": [
                           {
                             "principals": {
                               "values": ["foo"]
                             },
                             "reserver_principals": {
                               "values": ["foo", "bar"]
                             }
                           },
                           {
                             "principals": {
                               "values": ["bar"]
                             },
                             "reserver_principals": {
                               "values": ["bar"]
                             }
                           }
                         ]
}
```

The principal `foo` can create persistent volumes for any role, and no other
principal can create persistent volumes.

```json
{
  "permissive": false,
  "create_volumes": [
                      {
                        "principals": {
                          "values": ["foo"]
                        },
                        "roles": {
                          "type": "ANY"
                        }
                      }
                    ]
}
```

The principal `foo` cannot create persistent volumes for any role, and any
other principal can create persistent volumes for any role.

```json
{
  "create_volumes": [
                      {
                        "principals": {
                          "values": ["foo"]
                        },
                        "roles": {
                          "type": "NONE"
                        }
                      }
                    ]
}
```

The principal `foo` can create persistent volumes only for roles `prod` and
`dev`, and no other principal can create persistent volumes for any role.

```json
{
  "permissive": false,
  "create_volumes": [
                      {
                        "principals": {
                          "values": ["foo"]
                        },
                        "roles": {
                          "values": ["prod", "dev"]
                        }
                      }
                    ]
}
```

The principal `foo` can destroy volumes created by itself and by the principal
`bar`. The principal `bar`, however, can only destroy its own volumes. No other
principal can destroy volumes.

```json
{
  "permissive": false,
  "destroy_volumes": [
                       {
                         "principals": {
                           "values": ["foo"]
                         },
                         "creator_principals": {
                           "values": ["foo", "bar"]
                         }
                       },
                       {
                         "principals": {
                           "values": ["bar"]
                         },
                         "creator_principals": {
                           "values": ["bar"]
                         }
                       }
                     ]
}
```

The principal `ops` can query quota status for any role. The principal `foo`,
however, can only query quota status for `foo-role`. No other principal can
query quota status.

```json
{
  "permissive": false,
  "get_quotas": [
                  {
                    "principals": {
                      "values": ["ops"]
                    },
                    "roles": {
                      "type": "ANY"
                    }
                  },
                  {
                    "principals": {
                      "values": ["foo"]
                    },
                    "roles": {
                      "values": ["foo-role"]
                    }
                  }
                ]
}
```

The principal `ops` can update quota information (set or remove) for any role.
The principal `foo`, however, can only update quota for `foo-role`. No other
principal can update quota.

```json
{
  "permissive": false,
  "update_quotas": [
                     {
                       "principals": {
                         "values": ["ops"]
                       },
                       "roles": {
                         "type": "ANY"
                       }
                     },
                     {
                       "principals": {
                         "values": ["foo"]
                       },
                       "roles": {
                         "values": ["foo-role"]
                       }
                     }
                   ]
}
```

## Implementing an Authorizer

In case you plan to implement your own authorizer [module](modules.md), the
authorization interface consists of three parts:

First, the `authorization::Request` protobuf message represents a request to be
authorized. It follows the
_[Subject-Verb-Object](https://en.wikipedia.org/wiki/Subject%E2%80%93verb%E2%80%93object)_
pattern, where a _subject_ — commonly a principal — attempts to perform an
_action_ on a given _object_.

Second, the
`Future<bool> mesos::Authorizer::authorized(const mesos::authorization::Request& request)`
interface defines the entry point for authorizer modules (and the local
authorizer). A call to `authorized()` returns a future that indicates the result
of the (asynchronous) authorization operation. If the future is set to true, the
request was authorized successfully; if it was set to false, the request was
rejected. A failed future indicates that the request could not be processed at
the moment and it can be retried later.

The `authorization::Request` message is defined in authorizer.proto:

```protoc
message Request {
  optional Subject subject = 1;
  optional Action  action  = 2;
  optional Object  object  = 3;
}

message Subject {
  optional string value = 1;
}

message Object {
  optional string value = 1;
  optional FrameworkInfo framework_info = 2;
  optional Task task = 3;
  optional TaskInfo task_info = 4;
  optional ExecutorInfo executor_info = 5;
}
```

`Subject` or `Object` are optional fiels; if they are not set they
will only match an ACL with ANY or NONE in the
corresponding location. This allows users to construct the following requests:
_Can everybody perform action **A** on object **O**?_, or _Can principal **Z**
execute action **X** on all objects?_.

`Object` has several optional fields of which, depending on the action,
one or more fields must be set
(e.g., the `view_executors` action expects the `executor_info` and `framework_info` to be set).

The `action` field of the `Request` message is an enum. It is kept optional —
even though a valid action is necessary for every request — to allow for
backwards compatibility when adding new fields (see
[MESOS-4997](https://issues.apache.org/jira/browse/MESOS-4997) for details).

Third, the `ObjectApprover` interface. In order to support efficient authorization of large objects and multiple objects a user can request an `ObjectApprover` via `Future<Owned<ObjectApprover>> getObjectApprover(const authorization::Subject& subject, const authorization::Action& action)`.
The resulting `ObjectApprover` provides `Try<bool> approved(const ObjectApprover::Object& object)` to synchronously check whether objects are
authorized. The `ObjectApprover::Object` follows the structure of the `Request::Object` above.

```cpp
struct Object
{
  const std::string* value;
  const FrameworkInfo* framework_info;
  const Task* task;
  const TaskInfo* task_info;
  const ExecutorInfo* executor_info;
};
```

As the fields take pointer to each entity the `ObjectApprover::Object` does not require the entity to be copied.

NOTE: As the `ObjectApprover` is run synchronously in a different actor process
`ObjectApprover.approved()` call must not block!
