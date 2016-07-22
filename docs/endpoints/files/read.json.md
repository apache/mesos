---
title: Apache Mesos - HTTP Endpoints - /files/read.json
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /files/read.json

### TL;DR; ###
Reads data from a file.

### DESCRIPTION ###
This endpoint reads data from a file at a given offset and for
a given length.
Query parameters:

>        path=VALUE          The path of directory to browse.
>        offset=VALUE        Value added to base address to obtain a second address
>        length=VALUE        Length of file to read.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
Reading files requires that the request principal is 
authorized to do so for the target virtual file path.

Authorizers may categorize different virtual paths into
different ACLs, e.g. logs in one and task sandboxes in
another.

See authorization documentation for details.