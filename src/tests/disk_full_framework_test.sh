#!/usr/bin/env bash

# This script runs the disk full framework on a cluster using the disk/du
# isolator and checks that the framework returns a status of 1.

FILE="${MESOS_SOURCE_DIR}/support/colors.sh" && test -f $FILE && source $FILE
FILE="${MESOS_SOURCE_DIR}/support/atexit.sh" && test -f $FILE && source $FILE
FILE="${MESOS_HELPER_DIR}/colors.sh" && test -f $FILE && source $FILE
FILE="${MESOS_HELPER_DIR}/atexit.sh" && test -f $FILE && source $FILE

MESOS_WORK_DIR=$(mktemp -d -t mesos-XXXXXX)
export MESOS_WORK_DIR
atexit "rm -rf ${MESOS_WORK_DIR}"

MESOS_RUNTIME_DIR=$(mktemp -d -t mesos-XXXXXX)
export MESOS_RUNTIME_DIR
atexit "rm -rf ${MESOS_RUNTIME_DIR}"

# Disable support for systemd as this test does not run as root.
# This flag must be set as an environment variable because the flag
# does not exist on non-Linux builds.
export MESOS_SYSTEMD_ENABLE_SUPPORT=false

export MESOS_ISOLATION='disk/du'
export MESOS_ENFORCE_CONTAINER_DISK_QUOTA=1
export MESOS_RESOURCES="cpus:1;mem:96;disk:50"
export MESOS_CONTAINER_DISK_WATCH_INTERVAL="100ms"

# Lower the authentication timeout to speed up the test (the master
# may drop the authentication message while it is recovering).
export MESOS_AUTHENTICATION_TIMEOUT=200ms

# The main event!
"${MESOS_HELPER_DIR}"/disk-full-framework \
    --master=local \
    --pre_sleep_duration=1secs \
    --post_sleep_duration=30secs \
    --disk_use_limit=10mb \
    --run_once
STATUS=${?}

# Make sure the disk full framework "failed".
if [[ ! ${STATUS} -eq 1 ]]; then
  echo "${RED} Disk full framework returned ${STATUS} not 1${NORMAL}"
  exit 1
fi

exit 0
