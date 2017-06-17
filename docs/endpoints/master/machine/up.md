---
title: Apache Mesos - HTTP Endpoints - /machine/up
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /machine/up
>        /master/machine/up

### TL;DR; ###
Brings a set of machines back up.

### DESCRIPTION ###
Returns 200 OK when the operation was successful.

Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.

Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.

POST: Validates the request body as JSON and transitions
  the list of machines into UP mode.  This also removes
  the list of machines from the maintenance schedule.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
The current principal must be allowed to bring up all the machines
in the request, otherwise the request will fail.