#!/bin/sh

# Set local Mesos runner to use 3 slaves
export MESOS_SLAVES=3

# Check that the C test framework executes without crashing (returns 0).
exec $MESOS_HOME/bin/examples/test-framework local
