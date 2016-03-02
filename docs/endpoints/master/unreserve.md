---
title: Apache Mesos - HTTP Endpoints - /master/unreserve
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /master/unreserve

### TL;DR; ###
Unreserve resources dynamically on a specific slave.

### DESCRIPTION ###
Returns 200 OK if the request was accepted. This does not
imply that the requested resources have been unreserved successfully:
resource unreservation is done asynchronously and may fail.

Please provide "slaveId" and "resources" values designating
the resources to be unreserved.