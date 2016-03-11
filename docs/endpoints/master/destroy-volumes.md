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
Returns 200 OK if the request was accepted. This does not
imply that the volume was destroyed successfully: volume
destruction is done asynchronously and may fail.

Please provide "slaveId" and "volumes" values designating
the volumes to be destroyed.