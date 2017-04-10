---
title: Apache Mesos - HTTP Endpoints - /unreserve
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /unreserve
>        /master/unreserve

### TL;DR; ###
Unreserve resources dynamically on a specific agent.

### DESCRIPTION ###
Returns 202 ACCEPTED which indicates that the unreserve
operation has been validated successfully by the master.

Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.

Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.

The request is then forwarded asynchronously to the Mesos
agent where the reserved resources are located.
That asynchronous message may not be delivered or
unreserving resources at the agent might fail.

Please provide "slaveId" and "resources" values describing
the resources to be unreserved.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
Using this endpoint to unreserve resources requires that the
current principal is authorized to unreserve resources created
by the principal who reserved the resources.
See the authorization documentation for details.