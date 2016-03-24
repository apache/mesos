---
title: Apache Mesos - HTTP Endpoints - /api/v1/executor
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /api/v1/executor
>        /slave(1)/api/v1/executor

### TL;DR; ###
Endpoint for the Executor HTTP API.

### DESCRIPTION ###
This endpoint is used by the executors to interact with the
agent via Call/Event messages.
Returns 200 OK iff the initial SUBSCRIBE Call is successful.
This would result in a streaming response via chunked
transfer encoding. The executors can process the response
incrementally.
Returns 202 Accepted for all other Call messages iff the
request is accepted.


### AUTHENTICATION ###
This endpoint does not require authentication.