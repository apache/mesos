<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /metrics/snapshot

### TL;DR; ###
Provides a snapshot of the current metrics.

### DESCRIPTION ###
This endpoint provides information regarding the current metrics
tracked by the system.

The optional query parameter 'timeout' determines the maximum
amount of time the endpoint will take to respond. If the timeout
is exceeded, some metrics may not be included in the response.

The key is the metric name, and the value is a double-type.