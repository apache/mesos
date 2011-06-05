#!/bin/sh

# Check that the Java test framework executes without crashing (returns 0).
exec $MESOS_HOME/swig/java/test_framework local
