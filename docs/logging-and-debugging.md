---
layout: documentation
---

# Logging and Debugging

Mesos uses the [Google Logging library](http://code.google.com/p/google-glog) and writes logs to `MESOS_HOME/logs` by default, where `MESOS_HOME` is the location where Mesos is installed. The log directory can be [configured](configuration.md) using the `log_dir` parameter.

Frameworks that run on Mesos have their output stored to a "work" directory on each machine. By default, this is `MESOS_HOME/work`. Within this directory, a framework's output is placed in files called `stdout` and `stderr` in a directory of the form `slave-X/fw-Y/Z`, where X is the slave ID, Y is the framework ID, and multiple subdirectories Z are created for each attempt to run an executor for the framework. These files can also be accessed via the web UI of the slave daemon.