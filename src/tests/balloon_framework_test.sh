#!/bin/bash

# This script runs the balloon framework on a cluster using cgroups
# isolation module and checks that the framework returns a status of 1.

source ${MESOS_SOURCE_DIR}/support/colors.sh
source ${MESOS_SOURCE_DIR}/support/atexit.sh

# TODO(benh): Look for an existing hierarchy first.
HIERARCHY=/cgroup

# Check if the hierarchy exists. If it doesn't, we want to make sure we
# remove it, since we will create it.
unmount=false
if [[ ! -d ${HIERARCHY} ]]; then
  unmount=true
fi

MASTER_PID=
SLAVE_PID=

# This function ensures that we first kill the slave (if present) and
# then cleanup the cgroups. This is necessary because a running slave
# holds an advisory lock that disallows cleaning up cgroups.
# This function is not pure, but depends on state from the environment
# (e.g. ${SLAVE_PID}) because we do not all possible values about when we
# register with 'atexit'.
function cleanup() {
  # Make sure we kill the master on exit.
  if [[ ! -z ${MASTER_PID} ]]; then
    kill ${MASTER_PID}
  fi

  # Make sure we kill the slave on exit.
  if [[ ! -z ${SLAVE_PID} ]]; then
    kill ${SLAVE_PID}
  fi

  # Make sure we cleanup any cgroups we created on exiting.
  find ${HIERARCHY}/mesos/* -depth -type d | xargs -r rmdir

  # Make sure we cleanup the hierarchy, if we created it.
  # NOTE: We do a sleep here, to ensure the hierarchy is not busy.
  if ${unmount}; then
   rmdir ${HIERARCHY}/mesos; sleep 1; umount ${HIERARCHY}; rmdir ${HIERARCHY}
  fi
}

atexit cleanup

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
