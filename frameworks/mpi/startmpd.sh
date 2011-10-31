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

export PYTHONPATH=$MESOS_HOME/lib/python:$MESOS_HOME/third_party/protobuf-2.3.0/python:$PYTHONPATH
exec $PYTHON "$(dirname $0)/startmpd.py" $@
