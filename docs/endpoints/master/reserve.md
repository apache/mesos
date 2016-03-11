---
title: Apache Mesos - HTTP Endpoints - /reserve
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /reserve
>        /master/reserve

### TL;DR; ###
Reserve resources dynamically on a specific slave.

### DESCRIPTION ###
Returns 200 OK if the request was accepted. This does not
imply that the requested resources have been reserved successfully:
resource reservation is done asynchronously and may fail.

Please provide "slaveId" and "resources" values designating
the resources to be reserved.