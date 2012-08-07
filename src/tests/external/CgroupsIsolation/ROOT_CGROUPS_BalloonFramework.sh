#!/bin/bash

# This script runs the balloon framework on a cluster using cgroups isolation.

# Check that we're running as root
if [[ $EUID -ne 0 ]]; then
  echo "This test must be run as root." >&2
  exit 2
fi

# Launch master
$MESOS_BUILD_DIR/src/mesos-master --port=5432 > master.log 2>&1 &
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
$MESOS_BUILD_DIR/src/mesos-slave \
    --master=localhost:5432 \
    --isolation=cgroups \
    --resources="cpus:1;mem:96" \
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

# Launch balloon framework
echo "Running balloon framework"
$MESOS_BUILD_DIR/src/balloon-framework localhost:5432 \
  1024 > balloon.log 2>&1
EXIT_CODE=$?
echo "Balloon framework exit code: $?"
sleep 2

# Kill everything once balloon framework exited
echo "Killing slave: $SLAVE_PID"
kill $SLAVE_PID
sleep 2
echo "Killing master: $MASTER_PID"
kill $MASTER_PID
sleep 2

echo "Exiting"
# Check whether balloon framework returned the right code
if [[ $EXIT_CODE -eq 1 ]]; then
  exit 0
else
  exit 1
fi
