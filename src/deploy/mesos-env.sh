#!/usr/bin/env bash

DEPLOY_DIR=`dirname "$0"`
DEPLOY_DIR=`cd "$DEPLOY_DIR"; pwd`

# Locate MESOS_HOME relative to deploy directory
MESOS_HOME=`cd "$DEPLOY_DIR/.."; pwd`

# Find files that list masters and slaves
MASTERS_FILE="$MESOS_HOME/conf/masters"
if [ -e "$MASTERS_FILE" ]; then
  MASTERS=`cat "$MASTERS_FILE"`
else
  echo "Error: $MASTERS_FILE does not exist" >&2
  exit 1
fi
SLAVES_FILE="$MESOS_HOME/conf/slaves"
if [ -e "$SLAVES_FILE" ]; then
  SLAVES=`cat "$SLAVES_FILE"`
else
  echo "Error: $SLAVES_FILE does not exist" >&2
  exit 1
fi

# Find Mesos URL to use; first look to see if url is set in
# the Mesos config file, and if it isn't, use 1@MASTER:5050
# (taking the first master in the masters file)
MESOS_URL=`$MESOS_HOME/bin/mesos-getconf url`
if [ "x$MESOS_URL" == "x" ]; then
  FIRST_MASTER=`head -1 "$MASTERS_FILE"`
  MESOS_URL="mesos://1@$FIRST_MASTER:5050"
fi

# Read the deploy_with_sudo config setting to determine whether to run
# slave daemons as sudo.
DEPLOY_WITH_SUDO=`$MESOS_HOME/bin/mesos-getconf deploy_with_sudo`

# Options for SSH
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=2"

# Set LIBPROCESS_IP to change the address to which the master and slaves bind
# if the default address chosen by the system is not the right one. We include
# two examples below that try to resolve the IP from the node's hostname.
#LIBPROCESS_IP="hostname -i" #works on older versions of hostname, not on OS X
#FULL_IP="hostname --all-ip-addresses" # newer versions of hostname only
#export LIBPROCESS_IP=`echo $FULL_IP|sed 's/\([^ ]*\) .*/\1/'`
