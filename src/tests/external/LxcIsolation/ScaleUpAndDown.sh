#!/bin/bash

# Create scheduled-memhog's schedule file
echo "
0 30 200 200
10 10 200 200
" > schedule

export DESIRED_MEMHOG_EXIT_CODE=0

# Exec run_scheduled_memhog_test.sh, which actually runs the test and reports
# whether memhog gave the desired exit status through its exit status
exec $MESOS_HOME/tests/external/LxcIsolation/run_scheduled_memhog_test.sh
