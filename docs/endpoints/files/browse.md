---
title: Apache Mesos - HTTP Endpoints - /files/browse
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /files/browse

### TL;DR; ###
Returns a file listing for a directory.

### DESCRIPTION ###
Lists files and directories contained in the path as
a JSON object.

Query parameters:

>        path=VALUE          The path of directory to browse.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
Browsing files requires that the request principal is
authorized to do so for the target virtual file path.

Authorizers may categorize different virtual paths into
different ACLs, e.g. logs in one and task sandboxes in
another.

See authorization documentation for details.