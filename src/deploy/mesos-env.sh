#!/usr/bin/env bash

DEPLOY_DIR=`dirname "$0"`
DEPLOY_DIR=`cd "$DEPLOY_DIR"; pwd`

echo DEPLOY_DIR is $DEPLOY_DIR

#files that list master(s) and slaves
MASTER=`cat $DEPLOY_DIR/master`
SLAVES=`cat $DEPLOY_DIR/slaves`

#The dir where Mesos deployment scripts live
MESOS_HOME=`cd "$DEPLOY_DIR/.."; pwd`
echo "MESOS_HOME is $MESOS_HOME"

MESOS_LOGS=$MESOS_HOME/logs

#options for ssh'ing
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=2"

#LIBPROCESS_IP="hostname -i" #works on older versions of hostname, not on osx
#FULL_IP="hostname --all-ip-addresses" # newer versions of hostname only
#export LIBPROCESS_IP=`echo $FULL_IP|sed 's/\([^ ]*\) .*/\1/'`
