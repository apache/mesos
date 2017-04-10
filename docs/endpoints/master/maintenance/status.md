---
title: Apache Mesos - HTTP Endpoints - /maintenance/status
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /maintenance/status
>        /master/maintenance/status

### TL;DR; ###
Retrieves the maintenance status of the cluster.

### DESCRIPTION ###
Returns 200 OK when the maintenance status was queried successfully.

Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.

Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.

Returns an object with one list of machines per machine mode.
For draining machines, this list includes the frameworks' responses
to inverse offers.
**NOTE**:
Inverse offer responses are cleared if the master fails over.
However, new inverse offers will be sent once the master recovers.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.