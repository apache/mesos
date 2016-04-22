---
title: Apache Mesos - HTTP Endpoints - /quota
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /quota
>        /master/quota

### TL;DR; ###
Sets quota for a role.

### DESCRIPTION ###
Returns 200 OK when the quota has been changed successfully.
Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.
Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.
POST: Validates the request body as JSON
 and sets quota for a role.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.