#!/usr/bin/env bash

# This script runs the balloon framework on a cluster using the cgroups
# isolator and checks that the framework returns a status of 1.

FILE="${MESOS_SOURCE_DIR}/support/colors.sh" && test -f $FILE && source $FILE
FILE="${MESOS_SOURCE_DIR}/support/atexit.sh" && test -f $FILE && source $FILE
FILE="${MESOS_HELPER_DIR}/colors.sh" && test -f $FILE && source $FILE
FILE="${MESOS_HELPER_DIR}/atexit.sh" && test -f $FILE && source $FILE

EXISTING_MEMORY_HIERARCHY=$(cat /proc/mounts | grep memory | cut -f 2 -d ' ')
if [[ -n ${EXISTING_MEMORY_HIERARCHY} ]]; then
  # Strip off the subsystem component.
  TEST_CGROUP_HIERARCHY=${EXISTING_MEMORY_HIERARCHY%/*}
else
  TEST_CGROUP_HIERARCHY=/tmp/mesos_test_cgroup
fi
TEST_CGROUP_ROOT=mesos_test

# Check if the hierarchy exists. If it doesn't, we want to make sure we
# remove it, since we will create it.
unmount=false
if [[ ! -d ${TEST_CGROUP_HIERARCHY} ]]; then
  unmount=true
fi

MASTER_PID=
AGENT_PID=
MESOS_WORK_DIR=
MESOS_RUNTIME_DIR=

# This function ensures that we first kill the agent (if present) and
# then cleanup the cgroups. This is necessary because a running agent
# holds an advisory lock that disallows cleaning up cgroups.
# This function is not pure, but depends on state from the environment
# (e.g. ${AGENT_PID}) because we do not all possible values about when we
# register with 'atexit'.
function cleanup() {
  # Make sure we kill the master on exit.
  if [[ ! -z ${MASTER_PID} ]]; then
    kill ${MASTER_PID}
  fi

  # Make sure we kill the agent on exit.
  if [[ ! -z ${AGENT_PID} ]]; then
    kill ${AGENT_PID}
  fi

  # Make sure we cleanup any cgroups we created on exiting.
  find ${TEST_CGROUP_HIERARCHY}/*/${TEST_CGROUP_ROOT} -mindepth 1 -depth -type d -exec rmdir '{}' \+

  # Make sure we cleanup the hierarchy, if we created it.
  # NOTE: We do a sleep here, to ensure the hierarchy is not busy.
  if ${unmount}; then
   rmdir ${TEST_CGROUP_HIERARCHY}/${TEST_CGROUP_ROOT} && sleep 1 && umount ${TEST_CGROUP_HIERARCHY} && rmdir ${TEST_CGROUP_HIERARCHY}
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
BALLOON_FRAMEWORK=${MESOS_HELPER_DIR}/balloon-framework

# The mesos binaries expect MESOS_ prefixed environment variables
# to correspond to flags, so we unset these here.
unset MESOS_BUILD_DIR
unset MESOS_SOURCE_DIR
unset MESOS_HELPER_DIR
#unset MESOS_LAUNCHER_DIR # leave this so we can find mesos-fetcher.
unset MESOS_VERBOSE

MESOS_WORK_DIR=`mktemp -d -t mesos-XXXXXX`
MESOS_RUNTIME_DIR=`mktemp -d -t mesos-XXXXXX`

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

EXECUTOR_ENVIRONMENT_VARIABLES="{\"LD_LIBRARY_PATH\":\"${LD_LIBRARY_PATH}\"}"

# Launch agent.
${AGENT} \
    --work_dir=${MESOS_WORK_DIR} \
    --runtime_dir=${MESOS_RUNTIME_DIR} \
    --master=127.0.0.1:5432 \
    --isolation=cgroups/mem \
    --cgroups_hierarchy=${TEST_CGROUP_HIERARCHY} \
    --cgroups_root=${TEST_CGROUP_ROOT} \
    --executor_environment_variables=${EXECUTOR_ENVIRONMENT_VARIABLES} \
    --resources="cpus:1;mem:96" &
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
${BALLOON_FRAMEWORK} \
    --master=127.0.0.1:5432 \
    --task_memory_usage_limit=1024MB \
    --task_memory=32MB
STATUS=${?}

# Make sure the balloon framework "failed".
if [[ ! ${STATUS} -eq 1 ]]; then
  echo "${RED} Balloon framework returned ${STATUS} not 1${NORMAL}"
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
