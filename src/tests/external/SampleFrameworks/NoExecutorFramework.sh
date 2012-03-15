#!/bin/sh

# Expecting MESOS_SOURCE_DIR and MESOS_BUILD_DIR to be in environment.

env | grep MESOS_SOURCE_DIR >/dev/null

test $? != 0 && \
  echo "Failed to find MESOS_SOURCE_DIR in environment" && \
  exit 1

env | grep MESOS_BUILD_DIR >/dev/null

test $? != 0 && \
  echo "Failed to find MESOS_BUILD_DIR in environment" && \
  exit 1

# Set local Mesos runner to use 3 slaves
export MESOS_NUM_SLAVES=3

# Check that the C++ test framework executes without crashing (returns 0).
exec ${MESOS_BUILD_DIR}/src/no-executor-framework local
