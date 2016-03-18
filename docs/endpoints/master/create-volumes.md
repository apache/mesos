---
title: Apache Mesos - HTTP Endpoints - /create-volumes
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /create-volumes
>        /master/create-volumes

### TL;DR; ###
Create persistent volumes on reserved resources.

### DESCRIPTION ###
Returns 200 OK if the request was accepted. This does not
imply that the volume was created successfully: volume
creation is done asynchronously and may fail.

Please provide "slaveId" and "volumes" values designating
the volumes to be created.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.