#!/bin/bash

# This script runs the balloon framework on a cluster using cgroups
# isolation module and checks that the framework returns a status of 1.

source ${MESOS_SOURCE_DIR}/support/colors.sh
source ${MESOS_SOURCE_DIR}/support/atexit.sh

# TODO(benh): Look for an existing hierarchy first.
HIERARCHY=/cgroup

# Check if the hierarchy exists, if it doesn't we want to make sure we
# remove it if we create it.
if [[ ! -d ${HIERARCHY} ]]; then
    atexit "(rmdir ${HIERARCHY}/mesos; unmount ${HIERARCHY}; rmdir ${HIERARCHY})"
fi

# Launch master.
${MESOS_BUILD_DIR}/src/mesos-master --port=5432 &
MASTER_PID=${!}
echo "${GREEN}Launched master at ${MASTER_PID}${NORMAL}"
sleep 2

# Check the master is still running after 2 seconds.
kill -0 ${MASTER_PID} >/dev/null 2>&1
STATUS=${?}
if [[ ${STATUS} -ne 0 ]]; then
  echo "{RED}Master crashed; failing test${NORMAL}"
  exit 2
fi

# Make sure we kill the master on exit.
atexit "kill ${MASTER_PID}"

# Launch slave.
${MESOS_BUILD_DIR}/src/mesos-slave \
    --master=localhost:5432 \
    --isolation=cgroups \
    --cgroups_hierarchy_root=${HIERARCHY} \
    --resources="cpus:1;mem:96" &
SLAVE_PID=${!}
echo "${GREEN}Launched slave at ${SLAVE_PID}${NORMAL}"
sleep 2

# Check the slave is still running after 2 seconds.
kill -0 ${SLAVE_PID} >/dev/null 2>&1
STATUS=${?}
if [[ ${STATUS} -ne 0 ]]; then
  echo "${RED}Slave crashed; failing test${NORMAL}"
  exit 2
fi

# Make sure we kill the slave on exit.
atexit "kill ${SLAVE_PID}"

# Make sure we cleanup any cgroups we created on exiting.
atexit "find ${HIERARCHY}/mesos/* -depth -type d | xargs -r rmdir"

# The main event!
${MESOS_BUILD_DIR}/src/balloon-framework localhost:5432 1024
STATUS=${?}

# Make sure the balloon framework "failed".
if [[ ! ${STATUS} -eq 1 ]]; then
  exit 1
fi

# And make sure the slave is still running!
kill -0 ${SLAVE_PID} >/dev/null 2>&1
STATUS=${?}
if [[ ${STATUS} -ne 0 ]]; then
  echo "${RED}Slave crashed; failing test${NORMAL}"
  exit 2
fi
