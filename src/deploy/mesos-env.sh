#!/usr/bin/env bash

DEPLOY_DIR=`dirname "$0"`
DEPLOY_DIR=`cd "$DEPLOY_DIR"; pwd`

# Locate MESOS_HOME relative to deploy directory
MESOS_HOME=`cd "$DEPLOY_DIR/.."; pwd`

# Find files that list master and slaves
MASTER_FILE="$MESOS_HOME/conf/master"
if [ -e "$MASTER_FILE" ]; then
  MASTER=`cat "$MASTER_FILE"`
else
  echo "Error: $MASTER_FILE does not exist" >&2
  exit 1
fi
SLAVES_FILE="$MESOS_HOME/conf/slaves"
if [ -e "$SLAVES_FILE" ]; then
  SLAVES=`cat "$SLAVES_FILE"`
else
  echo "Error: $SLAVES_FILE does not exist" >&2
  exit 1
fi

# Options for SSH
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=2"

# Set LIBPROCESS_IP to change the address to which the master and slaves bind
# if the default address chosen by the system is not the right one. We include
# two examples below that try to resolve the IP from the node's hostname.
#LIBPROCESS_IP="hostname -i" #works on older versions of hostname, not on OS X
#FULL_IP="hostname --all-ip-addresses" # newer versions of hostname only
#export LIBPROCESS_IP=`echo $FULL_IP|sed 's/\([^ ]*\) .*/\1/'`
