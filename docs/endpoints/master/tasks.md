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
Lists known tasks.

Query parameters:

>        limit=VALUE          Maximum number of tasks returned (default is 100).
>        offset=VALUE         Starts task list at offset.
>        order=(asc|desc)     Ascending or descending sort order (default is descending).


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.