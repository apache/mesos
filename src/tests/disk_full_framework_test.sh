#!/usr/bin/env bash

# This script runs the disk full framework on a cluster using the disk/du
# isolator and checks that the framework returns a status of 1.

source ${MESOS_SOURCE_DIR}/support/colors.sh
source ${MESOS_SOURCE_DIR}/support/atexit.sh
source ${MESOS_HELPER_DIR}/colors.sh
source ${MESOS_HELPER_DIR}/atexit.sh

MASTER_PID=
AGENT_PID=
MESOS_WORK_DIR=`mktemp -d -t mesos-XXXXXX`
MESOS_RUNTIME_DIR=`mktemp -d -t mesos-XXXXXX`

function cleanup() {
  # Make sure we kill the master on exit.
  if [[ ! -z ${MASTER_PID} ]]; then
    kill ${MASTER_PID}
  fi

  # Make sure we kill the agent on exit.
  if [[ ! -z ${AGENT_PID} ]]; then
    kill ${AGENT_PID}
  fi

  if [[ -d "${MESOS_WORK_DIR}" ]]; then
    rm -rf ${MESOS_WORK_DIR};
  fi

  if [[ -d "${MESOS_RUNTIME_DIR}" ]]; then
    rm -rf ${MESOS_RUNTIME_DIR};
  fi
}

atexit cleanup

export LD_LIBRARY_PATH=${MESOS_BUILD_DIR}/src/.libs
MASTER=${MESOS_SBIN_DIR}/mesos-master
AGENT=${MESOS_SBIN_DIR}/mesos-agent
DISK_FULL_FRAMEWORK=${MESOS_HELPER_DIR}/disk-full-framework

# The mesos binaries expect MESOS_ prefixed environment variables
# to correspond to flags, so we unset these here.
unset MESOS_BUILD_DIR
unset MESOS_SOURCE_DIR
unset MESOS_HELPER_DIR
unset MESOS_VERBOSE

# Launch master.
${MASTER} \
    --ip=127.0.0.1 \
    --port=5432 \
    --work_dir=${MESOS_WORK_DIR} &
MASTER_PID=${!}
echo "${GREEN}Launched master at ${MASTER_PID}${NORMAL}"
sleep 2

# Check the master is still running after 2 seconds.
kill -0 ${MASTER_PID} >/dev/null 2>&1
STATUS=${?}
if [[ ${STATUS} -ne 0 ]]; then
  echo "${RED}Master crashed; failing test${NORMAL}"
  exit 2
fi

# Disable support for systemd as this test does not run as root.
# This flag must be set as an environment variable because the flag
# does not exist on non-Linux builds.
export MESOS_SYSTEMD_ENABLE_SUPPORT=false

# Launch agent.
${AGENT} \
    --work_dir=${MESOS_WORK_DIR} \
    --runtime_dir=${MESOS_RUNTIME_DIR} \
    --master=127.0.0.1:5432 \
    --isolation='disk/du' \
    --enforce_container_disk_quota \
    --resources="cpus:1;mem:96;disk:50" &
AGENT_PID=${!}
echo "${GREEN}Launched agent at ${AGENT_PID}${NORMAL}"
sleep 2

# Check the agent is still running after 2 seconds.
kill -0 ${AGENT_PID} >/dev/null 2>&1
STATUS=${?}
if [[ ${STATUS} -ne 0 ]]; then
  echo "${RED}Slave crashed; failing test${NORMAL}"
  exit 2
fi

# The main event!
${DISK_FULL_FRAMEWORK} \
    --master=127.0.0.1:5432 \
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

# And make sure the agent is still running!
kill -0 ${AGENT_PID} >/dev/null 2>&1
STATUS=${?}
if [[ ${STATUS} -ne 0 ]]; then
  echo "${RED}Slave crashed; failing test${NORMAL}"
  exit 2
fi

exit 0
