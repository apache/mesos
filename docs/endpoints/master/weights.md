---
title: Apache Mesos - HTTP Endpoints - /weights
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /weights
>        /master/weights

### TL;DR; ###
Updates weights for the specified roles.

### DESCRIPTION ###
Returns 200 OK when the weights update was successful.
Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.
Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.
PUT: Validates the request body as JSON
and updates the weights for the specified roles.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
Getting weight information for a certain role requires that the
current principal is authorized to get weights for the target role,
otherwise the entry for the target role could be silently filtered.
See the authorization documentation for details.