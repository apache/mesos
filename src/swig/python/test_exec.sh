#!/bin/sh
PYTHONPATH="$PYTHONPATH:`dirname $0`"
exec `dirname $0`/test_exec.py
