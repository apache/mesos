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

ZK_URL="local"
QUORUM=2

LOG_DIR=`mktemp -d -t mesos-XXXXXX`
atexit "rm -rf ${LOG_DIR}"

LOAD_FILE="${LOG_DIR}/load"
touch ${LOAD_FILE}
echo "1024" >> ${LOAD_FILE}
echo "10240" >> ${LOAD_FILE}
echo "102400" >> ${LOAD_FILE}

export MESOS_LOG_TOOL=${MESOS_BUILD_DIR}/src/mesos-log

# Check that the Java log executes without crashing (returns 0).
exec $MESOS_BUILD_DIR/src/examples/java/test-log ${ZK_URL} ${QUORUM} ${LOG_DIR} ${LOAD_FILE}
