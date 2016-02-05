<!--- This is an automatically generated file. DO NOT EDIT! --->
### USAGE ###
>        /logging/toggle

### TL;DR; ###
Sets the logging verbosity level for a specified duration.

### DESCRIPTION ###
The libprocess library uses [glog][glog] for logging. The library
only uses verbose logging which means nothing will be output unless
the verbosity level is set (by default it's 0, libprocess useslevels 1, 2, and 3).

**NOTE:** If your application uses glog this will also affect
your verbose logging.

Query parameters:

>        level=VALUE          Verbosity level (e.g., 1, 2, 3)
>        duration=VALUE       Duration to keep verbosity level
>                             toggled (e.g., 10secs, 15mins, etc.)


[glog]: https://code.google.com/p/google-glog