---
title: Apache Mesos - HTTP Endpoints - /redirect
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /redirect
>        /master/redirect

### TL;DR; ###
Redirects to the leading Master.

### DESCRIPTION ###
Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.

Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.

**NOTES:**
1. This is the recommended way to bookmark the WebUI when running multiple Masters.
2. This is broken currently "on the cloud" (e.g., EC2) as this will attempt to redirect to the private IP address, unless `advertise_ip` points to an externally accessible IP


### AUTHENTICATION ###
This endpoint does not require authentication.