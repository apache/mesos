---
layout: documentation
---

# Authorization

Authorization currently allows

 1. Frameworks to (re-)register with authorized _roles_.
 2. Frameworks to launch tasks/executors as authorized _users_.
 3. Authorized _principals_ to shutdown frameworks through the "/teardown" HTTP endpoint.
 4. Authorized _principals_ to set and remove quotas through the "/quota" HTTP endpoint.
 5. Authorized _principals_ to reserve and unreserve resources through the "/reserve" and "/unreserve" HTTP endpoints, as well as with the `RESERVE` and `UNRESERVE` offer operations.
 6. Authorized _principals_ to create and destroy persistent volumes through the "/create-volumes" and "/destroy-volumes" HTTP endpoints, as well as with the `CREATE` and `DESTROY` offer operations.


## ACLs

Authorization is implemented via Access Control Lists (ACLs). For each of the above cases, ACLs can be used to restrict access. Operators can setup ACLs in JSON format. See [authorizer.proto](https://github.com/apache/mesos/blob/master/include/mesos/authorizer/authorizer.proto) for details.

Each ACL specifies a set of `Subjects` that can perform an `Action` on a set of `Objects`.

The currently supported `Actions` are:

1. "register_frameworks": Register frameworks
2. "run_tasks": Run tasks/executors
3. "shutdown_frameworks": Shutdown frameworks
4. "set_quotas": Set quotas
5. "remove_quotas": Remove quotas
6. "reserve_resources": Reserve resources
7. "unreserve_resources": Unreserve resources
8. "create_volumes": Create persistent volumes
9. "destroy_volumes": Destroy persistent volumes

The currently supported `Subjects` are:

1. "principals"
	- Framework principals (used by "register_frameworks", "run_tasks", "reserve", "unreserve", "create_volumes", and "destroy_volumes" actions)
	- Usernames (used by "shutdown_frameworks", "set_quotas", "remove_quotas", "reserve", "unreserve", "create_volumes", and "destroy_volumes" actions)

The currently supported `Objects` are:

1. "roles": Resource [roles](roles.md) that framework can register with (used by "register_frameworks" and "set_quotas" actions)
2. "users": Unix user to launch the task/executor as (used by "run_tasks" actions)
3. "framework_principals": Framework principals that can be shutdown by HTTP POST (used by "shutdown_frameworks" actions).
4. "resources": Resources that can be reserved. Currently the only types considered by the default authorizer are `ANY` and `NONE` (used by "reserves" action).
5. "reserver_principals": Framework principals whose reserved resources can be unreserved (used by "unreserves" action).
6. "volume_types": Types of volumes that can be created by a given principal. Currently the only types considered by the default authorizer are `ANY` and `NONE` (used by "create_volumes" action).
7. "creator_principals": Principals whose persistent volumes can be destroyed (used by "destroy_volumes" action).
8. "quota_principals": Principals that set the quota to be removed (used by "remove_quotas" action)

> NOTE: Both `Subjects` and `Objects` can be either an array of strings or one of the special values `ANY` or `NONE`.


## How does it work?

The Mesos master checks the ACLs to verify whether a request is authorized or not.

For example, when a framework (re-)registers with the master, "register_frameworks" ACLs are checked to see if the framework (`FrameworkInfo.principal`) is authorized to receive offers for the given resource role (`FrameworkInfo.role`). If not authorized, the framework is not allowed to (re-)register and gets an `Error` message back (which aborts the scheduler driver).

Similarly, when a framework launches a task, "run_tasks" ACLs are checked to see if the framework (`FrameworkInfo.principal`) is authorized to run the task/executor as the given user. If not authorized, the launch is rejected and the framework gets a TASK_LOST.

In the same vein, when a user/principal attempts to shutdown a framework using the "/teardown" HTTP endpoint on the master, "shutdown_frameworks" ACLs are checked to see if the principal is authorized to shutdown the given framework. If not authorized, the shutdown is rejected and the user receives an `Unauthorized` HTTP response.

If no user/principal is provided in a request to an HTTP endpoint and authentication is disabled, the `ANY` subject is used in the authorization.

There are couple of important things to note:

1. ACLs are matched in the order that they are specified. In other words, the first matching ACL determines whether a request is authorized or not.

2. If no ACLs match a request, whether the request is authorized or not is determined by the `ACLs.permissive` field. This is "true" by default -- i.e., non-matching requests are authorized.


## Examples

1. Frameworks `foo` and `bar` can run tasks as user `alice`.

        {
          "run_tasks": [
                         {
                           "principals": { "values": ["foo", "bar"] },
                           "users": { "values": ["alice"] }
                         }
                       ]
        }

2. Any framework can run tasks as user `guest`.

        {
          "run_tasks": [
                         {
                           "principals": { "type": "ANY" },
                           "users": { "values": ["guest"] }
                         }
                       ]
        }

3. No framework can run tasks as `root`.

        {
          "run_tasks": [
                         {
                           "principals": { "type": "NONE" },
                           "users": { "values": ["root"] }
                         }
                       ]
        }

4. Framework `foo` can run tasks only as user `guest` and no other user.

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

5. Framework `foo` can register with the `analytics` and `ads` roles.

        {
          "register_frameworks": [
                                   {
                                     "principals": {
                                       "values": ["foo"]
                                     },
                                     "roles": {
                                       "values": ["analytics", "ads"]
                                     }
                                   }
                                 ]
        }

6. Only framework `foo` and no one else can register with the `analytics` role.

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

7. Framework `foo` can only register with the `analytics` role but no other roles. Also, no other framework can register with any roles or run tasks.

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

8. The `ops` principal can shutdown any framework using the "/teardown" HTTP endpoint. No other framework can register with any roles or run tasks.

        {
          "permissive": false,
          "shutdown_frameworks": [
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


## Configuring authorization

Authorization is configured by specifying the `--acls` flag when starting the master:

* `acls`:  The value could be a JSON-formatted string of ACLs
           or a file path containing the JSON-formatted ACLs used
           for authorization. Path could be of the form 'file:///path/to/file'
           or '/path/to/file'.
           See the ACLs protobuf in authorizer.proto for the expected format.

For more information on master command-line flags, see the
[configuration](configuration.md) page.
