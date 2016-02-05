<!--- This is an automatically generated file. DO NOT EDIT! --->
### USAGE ###
>        /master/redirect

### TL;DR; ###
Redirects to the leading Master.

### DESCRIPTION ###
This returns a 307 Temporary Redirect to the leading Master.
If no Master is leading (according to this Master), then the
Master will redirect to itself.

**NOTES:**
1. This is the recommended way to bookmark the WebUI when
running multiple Masters.
2. This is broken currently "on the cloud" (e.g. EC2) as
this will attempt to redirect to the private IP address, unless
advertise_ip points to an externally accessible IP