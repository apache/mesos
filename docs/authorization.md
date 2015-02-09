---
layout: documentation
---

# Authorization

Mesos 0.20.0 adds support for framework authorization. Authorization allows

 1. Frameworks to (re-)register with authorized `roles`.
 2. Frameworks to launch tasks/executors as authorized `users`.
 3. Authorized `principals` to shutdown framework(s) through "/shutdown" HTTP endpoint.


## ACLs

Authorization is implemented via Access Control Lists (ACLs). For each of the 3 cases described above there is a corresponding ACL(s) that can be set to restrict access. Operators can setup ACLs in JSON format. See [mesos.proto](https://github.com/apache/mesos/blob/master/include/mesos/mesos.proto) for details.

Each ACL specifies a set of `Subjects` that can perform an `Action` on a set of `Objects`.

The currently supported `Actions` are :

1. "register_frameworks" : Register Frameworks
2. "run_tasks" : Run tasks/executors
3. "shutdown_frameworks" : Shutdown frameworks

The currently supported `Subjects` are :

1. "principals"
	- Framework principals (used by "register_frameworks" and "run_tasks" actions)
	- Usernames (used by "shutdown_frameworks" action)

The currently supported `Objects` are :

1. "roles" : Resource roles that framework can register with (used by "register_frameworks" action)
2. "users" : Unix user to launch the task/executor as (used by "run_tasks" action)
3. "framework_principals" : Framework principals that can be shutdown by HTTP POST (used by "shutdown_frameworks" action).

> NOTE: Both `Subjects` and `Objects` can take a list of strings or special values (`ANY` or `NONE`).


## How does it work?

The Mesos master checks the ACLs to verify whether a request is authorized or not.

For example, when a framework (re-)registers with the master, the "register_frameworks" ACLs are checked to see if the framework (`FrameworkInfo.principal`) is authorized to receive offers for the given resource role (`FrameworkInfo.role`). If not authorized, the framework is not allowed to (re-)register and gets an `Error` message back (which aborts the scheduler driver).

Similarly, when a framework launches a task(s), "run_tasks" ACLs are checked to see if the framework (`FrameworkInfo.principal`) is authorized to run the task/executor as the given `user`. If not authorized, the launch is rejected and the framework gets a TASK_LOST.

In the same vein, when a user/principal attempts to shutdown a framework through the "/shutdown" HTTP endpoint on the master, "shutdown_frameworks" ACLs are checked to see if the `principal` is authorized to shutdown the given framework. If not authorized, the shutdown is rejected and the user receives an `Unauthorized` HTTP response.


There are couple of important things to note:

1. ACLs are matched in the order that they are setup. In other words, the first matching ACL determines whether a request is authorized or not.

2. If none of the specified ACLs match the given request, whether the request is authorized or not is defined by `ACLs.permissive` field. By default this is "true" i.e., a non-matching request is authorized.


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
                               "principals": { "values": [ "foo" ] },
                               "users": { "values": ["guest"] }
                             },
                             {
                               "principals": { "values": [ "foo" ] },
                               "users": { "type": "NONE" }
                             }
                           ]
            }




5. Framework `foo` can register with `analytics` and `ads` roles.

            {
              "register_frameworks": [
                                       {
                                         "principals": { "values": ["foo"] },
                                         "roles": { "values": ["analytics", "ads"] }
                                       }
                                     ]
            }


6. Only framework `foo` and no one else can register with `analytics` role.

            {
              "register_frameworks": [
                                       {
                                         "principals": { "values": ["foo"] },
                                         "roles": { "values": ["analytics"] }
                                       },
                                       {
                                         "principals": { "type": "NONE" },
                                         "roles": { "values": ["analytics"] }
                                       }
                                     ]
            }

7. Framework `foo` can only register with `analytics` role but no other roles. Also, no other framework can register with any roles.

            {
              "permissive" : false,

              "register_frameworks": [
                                       {
                                         "principals": { "values": ["foo"] },
                                         "roles": { "values": ["analytics"] }
                                       }
                                     ]
            }


8. Only `ops` principal can shutdown any frameworks through "/shutdown" HTTP endpoint.

            {
              "permissive" : false,

              "shutdown_frameworks": [
                                       {
                                         "principals": { "values": ["ops"] },
                                         "framework_principals": { "type": "ANY" }
                                       }
                                     ]
            }


## Enabling authorization.

As part of this feature, a new flag was added to the master.

* `acls` :  The value could be a JSON formatted string of ACLs
            or a file path containing the JSON formatted ACLs used
            for authorization. Path could be of the form 'file:///path/to/file'
            or '/path/to/file'.
            See the ACLs protobuf in mesos.proto for the expected format.


**For the complete list of master options: ./mesos-master.sh --help**
