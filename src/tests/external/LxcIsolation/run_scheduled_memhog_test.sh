#!/bin/bash

# This script runs scheduled-memhog with the schedule file in ./schedule on a
# cluster using Linux Containers isolation, and checks if it returns
# $DESIRED_MEMHOG_EXIT_CODE (set through the environment).
# It is invoked by the actual tests (TwoSeparateTasks.sh, etc) after creating
# the schedule file and setting DESIRED_MEMHOG_EXIT_CODE.

# Check that we're running as root
if [[ $EUID -ne 0 ]]; then
  echo "This test must be run as root because it uses Linux Containers." >&2
  exit 2
fi

# Launch master
$MESOS_HOME/mesos-master --port=5432 > master.log 2>&1 &
MASTER_PID=$!
echo "Launched master, PID = $MASTER_PID"
sleep 2

# Check if it's still running after 2 seconds
kill -0 $MASTER_PID
KILL_EXIT_CODE=$?
if [[ $KILL_EXIT_CODE -ne 0 ]]; then
  echo "Master crashed; failing test"
  exit 2
fi

# Launch slave
$MESOS_HOME/mesos-slave \
    --url=master@localhost:5432 \
    --isolation=lxc \
    --resources="cpus:2;mem:$[512*1024*1024]" \
    > slave.log 2>&1 &
SLAVE_PID=$!
echo "Launched slave, PID = $SLAVE_PID"
sleep 2

# Check if it's still running after 2 seconds
kill -0 $SLAVE_PID
KILL_EXIT_CODE=$?
if [[ $KILL_EXIT_CODE -ne 0 ]]; then
  echo "Slave crashed; failing test"
  kill $MASTER_PID
  exit 2
fi

# Launch memhog
echo "Running scheduled-memhog"
$MESOS_HOME/examples/scheduled-memhog master@localhost:5432 schedule > memhog.log 2>&1
EXIT_CODE=$?
echo "Memhog exit code: $?"
sleep 2

# Kill everything once memhog exited
echo "Killing slave: $SLAVE_PID"
kill $SLAVE_PID
sleep 2
echo "Killing master: $MASTER_PID"
kill $MASTER_PID
sleep 2

echo "Exiting"
# Check whether memhog returned the right code
if [[ $EXIT_CODE -eq $DESIRED_MEMHOG_EXIT_CODE ]]; then
  exit 0
else
  exit 1
fi
