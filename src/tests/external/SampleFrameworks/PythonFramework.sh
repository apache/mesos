#!/bin/sh

# Set local Mesos runner to use 3 slaves
export MESOS_NUM_SLAVES=3

# Check that the Python test framework executes without crashing (returns 0).
exec $MESOS_HOME/bin/examples/python/test_framework local
