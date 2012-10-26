#!/bin/bash

# Fork the proces that spawns processes in a chain
$MESOS_HOME/tests/process-spawn &

#Now get the pid of the above process

pid=`ps -e | grep process-spawn | awk '{print $1}'`

echo pid
