---
layout: documentation
---

# Authorization

Authorization currently allows

 1. Frameworks to (re-)register with authorized _roles_.
 2. Frameworks to launch tasks/executors as authorized _users_.
 3. Authorized _principals_ to teardown frameworks through the "/teardown" HTTP endpoint.
 4. Authorized _principals_ to set and remove quotas through the "/quota" HTTP endpoint.
 5. Authorized _principals_ to reserve and unreserve resources through the "/reserve" and "/unreserve" HTTP endpoints, as well as with the `RESERVE` and `UNRESERVE` offer operations.
 6. Authorized _principals_ to create and destroy persistent volumes through the "/create-volumes" and "/destroy-volumes" HTTP endpoints, as well as with the `CREATE` and `DESTROY` offer operations.


## ACLs

Authorization is implemented via Access Control Lists (ACLs). For each of the above cases, ACLs can be used to restrict access. Operators can setup ACLs in JSON format. See [authorizer.proto](https://github.com/apache/mesos/blob/master/include/mesos/authorizer/authorizer.proto) for details.

Each ACL specifies a set of `Subjects` that can perform an `Action` on a set of `Objects`.

The currently supported `Actions` are:

1. "register_frameworks": Register frameworks
2. "run_tasks": Run tasks/executors
3. "teardown_frameworks": Teardown frameworks
4. "set_quotas": Set quotas
5. "remove_quotas": Remove quotas
6. "reserve_resources": Reserve resources
7. "unreserve_resources": Unreserve resources
8. "create_volumes": Create persistent volumes
9. "destroy_volumes": Destroy persistent volumes

The currently supported `Subjects` are:

1. "principals"
	- Framework principals (used by "register_frameworks", "run_tasks", "reserve", "unreserve", "create_volumes", and "destroy_volumes" actions)
	- Usernames (used by "teardown_frameworks", "set_quotas", "remove_quotas", "reserve", "unreserve", "create_volumes", and "destroy_volumes" actions)

The currently supported `Objects` are:

1. "roles": Resource [roles](roles.md) that framework can register with (used by "register_frameworks" and "set_quotas" actions)
2. "users": Unix user to launch the task/executor as (used by "run_tasks" actions)
3. "framework_principals": Framework principals that can be torn down by HTTP POST (used by "teardown_frameworks" actions).
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

In the same vein, when a user/principal attempts to teardown a framework using the "/teardown" HTTP endpoint on the master, "teardown_frameworks" ACLs are checked to see if the principal is authorized to teardown the given framework. If not authorized, the teardown is rejected and the user receives a `Forbidden` HTTP response.

If no user/principal is provided in a request to an HTTP endpoint and authentication is disabled, the `ANY` subject is used in the authorization.

There are couple of important things to note:

1. ACLs are matched in the order that they are specified. In other words, the first matching ACL determines whether a request is authorized or not.

2. If no ACLs match a request, whether the request is authorized or not is determined by the `ACLs.permissive` field. This is "true" by default -- i.e., non-matching requests are authorized.


## Role vs. Principal

A principal identifies an entity (i.e., a framework or an operator) that interacts with Mesos. A role, on the other hand, is used to associate resources with frameworks in various ways. A useful, if not entirely precise, analogy can be made with user management in the Unix world: principals correspond to usernames, while roles approximately correspond to groups. For more information about roles, see the [role documentation](roles.md).

In a real-world organization, principals and roles might be used to represent various individuals or groups; for example, principals could correspond to people responsible for particular frameworks, while roles could correspond to departments within the organization which run frameworks on the cluster. To illustrate this point, consider a company that wants to allocate datacenter resources amongst multiple departments, one of which is the accounting department. Here is a possible scenario in which the accounting department launches a Mesos framework and then attempts to destroy a persistent volume:

* An accountant launches their framework, which authenticates with the Mesos master using its `principal` and `secret`. Here, let the framework principal be `payroll-framework`; this principal represents the trusted identity of the framework.
* The framework now sends a registration message to the master. This message includes a `FrameworkInfo` object containing a `principal` and a `role`; in this case, it will use the role `accounting`. The principal in this message must be `payroll-framework`, to match the one used by the framework for authentication.
* The master looks through its ACLs to see if it has a `RegisterFramework` ACL which authorizes the principal `payroll-framework` to register with the `accounting` role. It does find such an ACL, so the framework registers successfully. Now that the framework belongs to the `accounting` role, any [weights](roles.md), [reservations](reservation.md), [persistent volumes](persistent-volume.md), or [quota](quota.md) associated with the accounting department's role will apply. This allows operators to control the resource consumption of this department.
* Suppose the framework has created a persistent volume on a slave which it now wishes to destroy. The framework sends an `ACCEPT` call containing an offer operation which will `DESTROY` the persistent volume.
* However, datacenter operators have decided that they don't want the accounting frameworks to delete volumes. Rather, the operators will manually remove the accounting department's persistent volumes to ensure that no important financial data is deleted accidentally. To accomplish this, they have set a `DestroyVolume` ACL which asserts that the principal `payroll-framework` can destroy volumes created by a `creator_principal` of `NONE`; in other words, this framework cannot destroy persistent volumes, so the operation will be refused.


## Examples

1. Principals `foo` and `bar` can run tasks as the agent operating system user `alice` and no other user. No other principals can run tasks.

        {
          "permissive": false,
          "run_tasks": [
                         {
                           "principals": { "values": ["foo", "bar"] },
                           "users": { "values": ["alice"] }
                         }
                       ]
        }

2. Principal `foo` can run tasks only as the agent operating system user `guest` and no other user. Any other principal (or framework without a principal) can run tasks as any user.

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

3. Any principal can run tasks as the agent operating system user `guest`. Tasks cannot be run as any other user.

        {
          "permissive": false,
          "run_tasks": [
                         {
                           "principals": { "type": "ANY" },
                           "users": { "values": ["guest"] }
                         }
                       ]
        }

4. No principal can run tasks as the agent operating system user `root`. Any principal (or framework without a principal) can run tasks as any other user.

        {
          "run_tasks": [
                         {
                           "principals": { "type": "NONE" },
                           "users": { "values": ["root"] }
                         }
                       ]
        }

5. Principal `foo` can register frameworks with the `analytics` and `ads` roles and no other role. Any other principal (or framework without a principal) can register frameworks with any role.

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

6. Only principal `foo` and no one else can register frameworks with the `analytics` role. Any other principal (or framework without a principal) can register frameworks with any other role.

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

7. Principal `foo` can register frameworks with the `analytics` role and no other role. No other principal can register frameworks with any role, including `*`.

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

8. The `ops` principal can teardown any framework using the "/teardown" HTTP endpoint. No other principal can teardown any frameworks.

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

9. The principal `foo` can reserve any resources, and no other principal can reserve resources.

        {
          "permissive": false,
          "reserve_resources": [
                                 {
                                   "principals": {
                                     "values": ["foo"]
                                   },
                                   "resources": {
                                     "type": "ANY"
                                   }
                                 }
                               ]
        }

10. The principal `foo` cannot reserve any resources, and any other principal (or framework without a principal) can reserve resources.

        {
          "reserve_resources": [
                                 {
                                   "principals": {
                                     "values": ["foo"]
                                   },
                                   "resources": {
                                     "type": "NONE"
                                   }
                                 }
                               ]
        }

11. The principal `foo` can unreserve resources reserved by itself and by the principal `bar`. The principal `bar`, however, can only unreserve its own resources. No other principals can unreserve resources.

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

12. The principal `foo` can create persistent volumes, and no other principal can create persistent volumes.

        {
          "permissive": false,
          "create_volumes": [
                              {
                                "principals": {
                                  "values": ["foo"]
                                },
                                "volume_types": {
                                  "type": "ANY"
                                }
                              }
                            ]
        }

13. The principal `foo` can destroy volumes created by itself and by the principal `bar`. The principal `bar`, however, can only destroy its own volumes. No other principals can destroy volumes.

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

14. The principal `ops` can set quota for any role. The principal `foo`, however, can only set quota for `foo-role`. No other principals can set quota.

        {
          "permissive": false,
          "set_quotas": [
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

15. The principal `ops` can remove quota which was set by any principal. The principal `foo`, however, can only remove quota which was set by itself. No other principals can remove quota.

        {
          "permissive": false,
          "remove_quotas": [
                             {
                               "principals": {
                                 "values": ["ops"]
                               },
                               "quota_principals": {
                                 "type": "ANY"
                               }
                             },
                             {
                               "principals": {
                                 "values": ["foo"]
                               },
                               "quota_principals": {
                                 "values": ["foo"]
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
