---
layout: documentation
---

# Authorization

Mesos 0.20.0 adds support for framework authorization. Authorization allows

 1. Frameworks to only launch tasks/executors as authorized `users`.
 2. Frameworks to be able to receive offers for authorized `roles`.
 3. HTTP endpoints exposed by Mesos to be accessible to authorized `clients`.

> NOTE: While ACLs support for HTTP is present, currently access to the HTTP endpoints are not authorized.

## ACLs

Authorization is implemented via Access Control Lists (ACLs). For each of the 3 cases described above there is a corresponding ACL(s) that can be set to restrict access. Operators can setup ACLs in JSON format. See [mesos.proto](https://github.com/apache/mesos/blob/master/include/mesos/mesos.proto) for details.

Each ACL specifies a set of `Subjects` that can perform an `Action` on a set of `Objects`.

The currently supported `Actions` are :

1. "run_tasks" : Run tasks/executors
2. "receive_offers" : Receive offers
3. "http_get" : HTTP GET access
4. "http_put" : HTTP_PUT access

The currently supported `Subjects` are :

1. "principals" : Framework principals (used by "run_tasks" and "receive_offers" actions)
2. "usernames" : Username used in HTTP Basic/Digest authentication. (used by "http_get" and "http_put" actions)
3. "ips" : IP Addresses of the clients (used by "http_get" and "http_put" actions)
4. "hostnames" : Hostnames of the clients (used by "http_get" and "http_put" actions)

The currently supported `Objects` are :

1. "users" : Unix user to launch the task/executor as (used by "run_tasks" action)
2. "roles" : Resource roles to receive offers from (used by "receive_offers" action)
3. "urls" : HTTP URL endpoint exposed by the master (used by "http_get" and "http_put" actions)

> NOTE: Both `Subjects` and `Objects` can take a list of strings or special values (`ANY` and `NONE`).


## How does it work?

The Mesos master checks the ACLs to verify whether a request is authorized or not. For example, when  a framework launches a task, "run_tasks" ACLs are checked to see if the framework (`FrameworkInfo.principal`) is authorized to run the task/executor as the given user. If not authorized, the launch is rejected and the framework gets a TASK_LOST.

Similarly, when a framework (re-)registers the Mesos master checks whether it is authorized to receive offers for given resource role (`FrameworkInfo.role`). If not authorized, the framework is not allowed to (re-)register and gets an Error message back.

While not yet implemented, GET/PUT access to HTTP endpoints exposed by the Mesos master will be authorized in a similar way.

There are couple of important things to note:

1. ACLs are matched in the order that they are setup. In other words, the first matching ACL determines whether a request is authorized or not.

2. If none of the specified ACLs match the given request, whether the request is authorized or not is defined by `ACLs.permissive` field. By default this "true" i.e., a non-matching request is authorized.


## Examples

1. Frameworks `foo` and `bar` can run tasks as user `alice`.

            {
              "run_tasks": [
                             {
                               "principals": { "values": ["foo", "bar"] },
                               "users": { "values": ["alice"] }
                             }
                           ],
            }

2. Any framework can run tasks as user `guest`.

            {
              "run_tasks": [
                             {
                               "principals": { "type": "ANY" },
                               "users": { "values": ["guest"] }
                             }
                           ],
            }

3. No framework can run tasks as `root`.

            {
              "run_tasks": [
                             {
                               "principals": { "type": "NONE" },
                               "users": { "values": ["root"] }
                             }
                           ],
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
                               "users": { "type": ["NONE"] }
                             }
                           ],
            }




5. Framework `foo` can be offered resources for `analytics` and `ads` roles.

            {
              "receive_offers": [
                                  {
                                    "principals": { "values": ["foo"] },
                                    "roles": { "values": ["analytics", "ads"] }
                                  }
                                ],
            }


6. Only framework `foo` and no one else can be offered resources for `analytics` role.

            {
              "receive_offers": [
                                  {
                                    "principals": { "values": ["foo"] },
                                    "roles": { "values": ["analytics"] }
                                  },
                                  {
                                    "principals": { "type": "NONE" },
                                    "roles": { "values": ["analytics"] }
                                  }
                                ],
            }

7. Framework `foo` can only receive offers for `analytics` role but no other roles. Also, no other framework can receive offers for any role.

            {
              "permissive" : "false",

              "receive_offers": [
                                  {
                                    "principals": { "values": ["foo"] },
                                    "roles": { "values": ["analytics"] }
                                  }
                                ],
            }


## Enabling authorization.

As part of this feature, a new flag was added to the master.

* `acls` :  The value could be a JSON formatted string of ACLs
            or a file path containing the JSON formatted ACLs used
            for authorization. Path could be of the form 'file:///path/to/file'
            or '/path/to/file'.
            See the ACLs protobuf in mesos.proto for the expected format.

            Example:
            {
              "run_tasks": [
                             {
                               "principals": { "values": ["a", "b"] },
                               "users": { "values": ["root"] }
                             }
                           ],
              "receive_offers": [
                                  {
                                    "principals": { "type": "ANY" },
                                    "roles": { "values": ["foo"] }
                                  }
                                ]
            }

**For the complete list of master options: ./mesos-master.sh --help**
