#!/bin/bash

# Create scheduled-memhog's schedule file. In this schedule, we launch two
# tasks that each claim to require 200 MB, but the first task only uses 50 MB
# while the second uses 350. While both tasks are running, this is fine, but
# once the first task finishes, the framework's memory can't be scaled down
# below 350 and so the LXC isolation module should kill it.
echo "
0 10 200 50
0 30 200 350
" > schedule

# Expect exit code 1 (returned by scheduled-memhog if some of its tasks fail)
export DESIRED_MEMHOG_EXIT_CODE=1

# Exec run_scheduled_memhog_test.sh, which actually runs the test and reports
# whether memhog gave the desired exit status through its exit status
exec $MESOS_HOME/tests/external/LxcIsolation/run_scheduled_memhog_test.sh
