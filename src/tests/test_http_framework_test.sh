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

FILE="${MESOS_SOURCE_DIR}/support/colors.sh" && test -f $FILE && source $FILE
FILE="${MESOS_SOURCE_DIR}/support/atexit.sh" && test -f $FILE && source $FILE
FILE="${MESOS_HELPER_DIR}/colors.sh" && test -f $FILE && source $FILE
FILE="${MESOS_HELPER_DIR}/atexit.sh" && test -f $FILE && source $FILE

MESOS_WORK_DIR=`mktemp -d -t mesos-XXXXXX`

atexit "rm -rf ${MESOS_WORK_DIR}"
export MESOS_WORK_DIR=${MESOS_WORK_DIR}

MESOS_RUNTIME_DIR=`mktemp -d -t mesos-XXXXXX`

atexit "rm -rf ${MESOS_RUNTIME_DIR}"
export MESOS_RUNTIME_DIR=${MESOS_RUNTIME_DIR}

# Lower the authentication timeout to speed up the test (the master
# may drop the authentication message while it is recovering).
export MESOS_AUTHENTICATION_TIMEOUT=200ms

# Set local Mesos runner to use 3 slaves
export MESOS_NUM_SLAVES=3

# Set resources for the slave.
export MESOS_RESOURCES="cpus:2;mem:10240"

# Set isolation for the slave.
export MESOS_ISOLATION="filesystem/posix,posix/cpu,posix/mem"

# Set launcher for the slave.
export MESOS_LAUNCHER="posix"

# Disable authentication as the scheduler library does not support it.
export MESOS_AUTHENTICATE_FRAMEWORKS=false

# Check that the C++ HTTP scheduler executes without crashing (returns 0).
exec ${MESOS_HELPER_DIR}/test-http-framework --master=local
