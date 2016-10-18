---
title: Apache Mesos - HTTP Endpoints - /files/download
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /files/download

### TL;DR; ###
Returns the raw file contents for a given path.

### DESCRIPTION ###
This endpoint will return the raw file contents for the
given path.

Query parameters:

>        path=VALUE          The path of directory to browse.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
Downloading files requires that the request principal is
authorized to do so for the target virtual file path.

Authorizers may categorize different virtual paths into
different ACLs, e.g. logs in one and task sandboxes in
another.

See authorization documentation for details.