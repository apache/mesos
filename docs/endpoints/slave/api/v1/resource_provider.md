---
title: Apache Mesos - HTTP Endpoints - /api/v1/resource_provider
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /api/v1/resource_provider
>        /slave(1)/api/v1/resource_provider

### TL;DR; ###
Endpoint for the local resource provider HTTP API.

### DESCRIPTION ###
This endpoint is used by the local resource providers to interact
with the agent via Call/Event messages.

Returns 200 OK iff the initial SUBSCRIBE Call is successful. This
will result in a streaming response via chunked transfer encoding.
The local resource providers can process the response incrementally.

Returns 202 Accepted for all other Call messages iff the request is
accepted.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.