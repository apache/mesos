#!/bin/bash

# This file contains environment variables that should be set when
# starting a Mesos daemon (master or slave) with the deploy
# scripts. It can be used to configure SSH options or set which IP
# addresses the daemons should bind to, for example.

# Options for SSH
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=2"

# Set LIBPROCESS_IP to change the address to which the master and
# slaves bind if the default address chosen by the system is not the
# right one. We include two examples below that try to resolve the IP
# from the node's hostname.

# This works with older versions of hostname, but not on Mac OS X.
#LIBPROCESS_IP="hostname -i" 

# This works with a newer version of hostname on Ubuntu.
#FULL_IP="hostname --all-ip-addresses"
#export LIBPROCESS_IP=`echo $FULL_IP | sed 's/\([^ ]*\) .*/\1/'`
