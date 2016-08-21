#!/usr/bin/env bash

# Expecting MESOS_SOURCE_DIR and MESOS_BUILD_DIR to be in environment.

env | grep MESOS_SOURCE_DIR >/dev/null

test $? != 0 && \
  echo "Failed to find MESOS_SOURCE_DIR in environment" && \
  exit 1

env | grep MESOS_BUILD_DIR >/dev/null

test $? != 0 && \
  echo "Failed to find MESOS_BUILD_DIR in environment" && \
  exit 1

source ${MESOS_SOURCE_DIR}/support/atexit.sh

MESOS_WORK_DIR=`mktemp -d -t mesos-XXXXXX`

atexit "rm -rf ${MESOS_WORK_DIR}"
export MESOS_WORK_DIR=${MESOS_WORK_DIR}

MESOS_RUNTIME_DIR=`mktemp -d -t mesos-XXXXXX`

atexit "rm -rf ${MESOS_RUNTIME_DIR}"
export MESOS_RUNTIME_DIR=${MESOS_RUNTIME_DIR}

# Set the connection delay to 0 to speed up the tests.
export MESOS_CONNECTION_DELAY_MAX=0ms;

# Set local Mesos runner to use 3 slaves.
export MESOS_NUM_SLAVES=3

# Set resources for the slave.
export MESOS_RESOURCES="cpus:2;mem:10240"

# Set isolation for the slave.
export MESOS_ISOLATION="filesystem/posix,posix/cpu,posix/mem"

# Set launcher for the slave.
export MESOS_LAUNCHER="posix"

# Check that the Java test framework executes without crashing (returns 0).
exec $MESOS_BUILD_DIR/src/examples/java/v1-test-framework local 1
