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
  MESOS_URL="mesos://master@$FIRST_MASTER:5050"
fi

# Read the deploy_with_sudo config setting to determine whether to run
# slave daemons as sudo.
DEPLOY_WITH_SUDO=`$MESOS_HOME/bin/mesos-getconf deploy_with_sudo`

# Read any user-configurable environment variables set in the deploy-env.sh file
# in MESOS_HOME/conf, such as SSH options.
if [ -e $MESOS_HOME/conf/deploy-env.sh ]; then
  . $MESOS_HOME/conf/deploy-env.sh
fi
