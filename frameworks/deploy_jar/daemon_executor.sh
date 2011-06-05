#!/bin/bash

PYTHON=python

if [ "`uname`" == "SunOS" ]; then
  PYTHON=python2.6
fi

export PYTHONPATH=`dirname $0`/../../src/swig/python:$PYTHONPATH

$PYTHON `dirname $0`/daemon_executor.py $@
