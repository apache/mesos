---
title: Apache Mesos - HTTP Endpoints - /frameworks
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /frameworks
>        /master/frameworks

### TL;DR; ###
Exposes the frameworks info.

### DESCRIPTION ###
Returns 200 OK when the frameworks info was queried successfully.

Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.

Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.

Query parameters:
>        framework_id=VALUE   The ID of the framework returned (if no framework ID is specified, all frameworks will be returned).


### AUTHENTICATION ###
This endpoint requires authentication if HTTP authentication is
enabled.

### AUTHORIZATION ###
This endpoint might be filtered based on the user accessing it.
See the authorization documentation for details.