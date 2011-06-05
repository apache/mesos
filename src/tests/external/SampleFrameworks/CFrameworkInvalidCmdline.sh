#!/bin/sh

# Run the C framework with an invalid slaves parameter (not an integer)
# and check that it reports the error.
$MESOS_HOME/bin/examples/test-framework --url=local --num_slaves=blah > framework.out 2>&1
if grep -e "Configuration error" framework.out; then
  exit 0
else
  exit 1
fi
