---
title: Apache Mesos - HTTP Endpoints - /reserve
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /reserve
>        /master/reserve

### TL;DR; ###
Reserve resources dynamically on a specific agent.

### DESCRIPTION ###
Returns 202 ACCEPTED which indicates that the reserve
operation has been validated successfully by the master.
Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.
Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.
The request is then forwarded asynchronously to the Mesos
agent where the reserved resources are located.
That asynchronous message may not be delivered or
reserving resources at the agent might fail.

Please provide "slaveId" and "resources" values designating
the resources to be reserved.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
Using this endpoint to reserve resources requires that the
current principal is authorized to reserve resources for the
specific role.
See the authorization documentation for details.