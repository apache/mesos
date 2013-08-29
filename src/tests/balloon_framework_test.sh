#!/bin/bash

# This script runs the balloon framework on a cluster using the cgroups
# isolator and checks that the framework returns a status of 1.

if [ $# -ne 3 ]; then
  echo "usage: `basename $0` cgroups_oom_buffer balloon_step balloon_limit"
  echo "example: `basename $0` 127MB 32MB 1GB"
  exit 1
fi

source ${MESOS_SOURCE_DIR}/support/colors.sh
source ${MESOS_SOURCE_DIR}/support/atexit.sh

# TODO(benh): Look for an existing hierarchy first.
TEST_CGROUP_HIERARCHY=/tmp/mesos_test_cgroup
TEST_CGROUP_ROOT=mesos_test

# Check if the hierarchy exists. If it doesn't, we want to make sure we
# remove it, since we will create it.
unmount=false
if [[ ! -d ${TEST_CGROUP_HIERARCHY} ]]; then
  unmount=true
fi

MASTER_PID=
SLAVE_PID=
SLAVE_WORK_DIR=

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
  find ${TEST_CGROUP_HIERARCHY}/${TEST_CGROUP_ROOT} -mindepth 1 -depth -type d | xargs -r rmdir

  # Make sure we cleanup the hierarchy, if we created it.
  # NOTE: We do a sleep here, to ensure the hierarchy is not busy.
  if ${unmount}; then
   rmdir ${TEST_CGROUP_HIERARCHY}/${TEST_CGROUP_ROOT} && sleep 1 && umount ${TEST_CGROUP_HIERARCHY} && rmdir ${TEST_CGROUP_HIERARCHY}
  fi

  if [[ -d "${SLAVE_WORK_DIR}" ]]; then
    rm -rf ${SLAVE_WORK_DIR};
  fi
}

atexit cleanup

export LD_LIBRARY_PATH=${MESOS_BUILD_DIR}/src/.libs
MASTER=${MESOS_BUILD_DIR}/src/mesos-master
SLAVE=${MESOS_BUILD_DIR}/src/mesos-slave
BALLOON_FRAMEWORK=${MESOS_BUILD_DIR}/src/balloon-framework

# The mesos binaries expect MESOS_ prefixed environment variables
# to correspond to flags, so we unset these here.
unset MESOS_BUILD_DIR
unset MESOS_SOURCE_DIR
unset MESOS_LAUNCHER_DIR
unset MESOS_VERBOSE

# Launch master.
${MASTER} --port=5432 &
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
SLAVE_WORK_DIR=`mktemp -d`

${SLAVE} \
    --work_dir=${SLAVE_WORK_DIR} \
    --master=localhost:5432 \
    --isolation=cgroups \
    --cgroups_hierarchy=${TEST_CGROUP_HIERARCHY} \
    --cgroups_root=${TEST_CGROUP_ROOT} \
    --resources="cpus:1;mem:96" \
    --cgroups_oom_buffer=${1} &
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
${BALLOON_FRAMEWORK} localhost:5432 ${2} ${3}
STATUS=${?}

# Make sure the balloon framework "failed".
if [[ ! ${STATUS} -eq 1 ]]; then
  echo "${RED} Balloon framework returned ${STATUS} not 1${NORMAL}"
  exit 1
fi

# And make sure the slave is still running!
kill -0 ${SLAVE_PID} >/dev/null 2>&1
STATUS=${?}
if [[ ${STATUS} -ne 0 ]]; then
  echo "${RED}Slave crashed; failing test${NORMAL}"
  exit 2
fi

exit 0
