---
title: Apache Mesos - HTTP Endpoints - /state-summary
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /state-summary
>        /master/state-summary

### TL;DR; ###
Summary of state of all tasks and registered frameworks in cluster.

### DESCRIPTION ###
Returns 200 OK when a summary of the master's state was queried
successfully.
Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.
Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.
This endpoint gives a summary of the state of all tasks and
registered frameworks in the cluster as a JSON object.
The information shown might be filtered based on the user
accessing the endpoint.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
This endpoint might be filtered based on the user accessing it.
For example a user might only see the subset of frameworks
they are allowed to view.
See the authorization documentation for details.