---
title: Apache Mesos - HTTP Endpoints - /files/read
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /files/read

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