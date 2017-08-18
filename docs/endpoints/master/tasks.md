---
title: Apache Mesos - HTTP Endpoints - /tasks
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /tasks
>        /master/tasks

### TL;DR; ###
Lists tasks from all active frameworks.

### DESCRIPTION ###
Returns 200 OK when task information was queried successfully.

Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.

Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.

Lists known tasks.
The information shown might be filtered based on the user
accessing the endpoint.

Query parameters:

>        framework_id=VALUE   Only return tasks belonging to the framework with this ID.
>        limit=VALUE          Maximum number of tasks returned (default is 100).
>        offset=VALUE         Starts task list at offset.
>        order=(asc|desc)     Ascending or descending sort order (default is descending).
>        task_id=VALUE        Only return tasks with this ID (should be used together with parameter 'framework_id').


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
This endpoint might be filtered based on the user accessing it.
For example a user might only see the subset of tasks they are
allowed to view.
See the authorization documentation for details.