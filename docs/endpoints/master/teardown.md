---
title: Apache Mesos - HTTP Endpoints - /teardown
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /teardown
>        /master/teardown

### TL;DR; ###
Tears down a running framework by shutting down all tasks/executors and removing the framework.

### DESCRIPTION ###
Returns 200 OK if the framework was torn down successfully.

Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.

Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.

Please provide a "frameworkId" value designating the running
framework to tear down.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
Using this endpoint to teardown frameworks requires that the
current principal is authorized to teardown frameworks created
by the principal who created the framework.
See the authorization documentation for details.