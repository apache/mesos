---
title: Apache Mesos - HTTP Endpoints - /destroy-volumes
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /destroy-volumes
>        /master/destroy-volumes

### TL;DR; ###
Destroy persistent volumes.

### DESCRIPTION ###
Returns 202 ACCEPTED which indicates that the destroy
operation has been validated successfully by the master.
Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.
Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.
The request is then forwarded asynchronously to the Mesos
agent where the reserved resources are located.
That asynchronous message may not be delivered or
destroying the volumes at the agent might fail.

Please provide "slaveId" and "volumes" values designating
the volumes to be destroyed.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
Using this endpoint to destroy persistent volumes requires that
the current principal is authorized to destroy volumes created
by the principal who created the volume.
See the authorization documentation for details.