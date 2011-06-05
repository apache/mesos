#!/bin/sh

# Check that the C++ test framework executes without crashing (returns 0).
exec $MESOS_HOME/cpp-test-framework local
