#!/bin/sh

# Check that the C test framework executes without crashing (returns 0).
exec $MESOS_HOME/bin/examples/test-framework --url=local --num_slaves=3
