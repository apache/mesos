#!/bin/bash

if [ "x$PYTHON" == "x" ]; then
  PYTHON=python
  if [ "`uname`" == "SunOS" ]; then
    PYTHON=python2.6
  fi
fi

if [ "x$MESOS_HOME" == "x" ]; then
  MESOS_HOME="$(dirname $0)/../.."
fi

export PYTHONPATH=$MESOS_HOME/lib/python
exec $PYTHON "$(dirname $0)/torquesched.py" $@
