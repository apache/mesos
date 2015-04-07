#!/usr/bin/setsid bash

# TODO(xujyan): Make the script OSX compatible.
function usage {
cat <<EOF

Run Mesos tests within a duration and attach gdb if the tests time out. This
script will run the test command under the current work directory. To put the
time limit only on tests, compile the project before running the script. This
script works only on Linux.

Usage: $0 [-h] <test_cmd> <duration>

  test_cmd   Command that runs the tests.
             e.g., MESOS_VERBOSE=1 make check GTEST_SHUFFLE=1
  duration   Duration (in seconds) before the tests time out.
             e.g., 3600, \$((160 * 60))
  -h         Print this help message and exit
EOF
}

die () {
  echo >&2 "$@"
  exit 1
}

if test ${#} -ne 2; then
  usage
  exit 1
fi

test_cmd=$1
duration=$2

echo "This script runs with session id $$ and can be terminated by: pkill -s $$"

printf "$(date)"; echo ": start running $test_cmd"

start=$(date +"%s")
eval $test_cmd &

test_cmd_pid=$!

while [ $(($(date +"%s") - $start)) -lt $duration ]; do
  running=`ps p $test_cmd_pid h | wc -l`
  if [ $running -eq 0 ]; then
    echo "Test finished"
    # Get the exit status.
    wait $test_cmd_pid
    exit_status=$?
    echo "Exit status: $exit_status"
    exit $exit_status
  fi

  sleep 5
done

printf "$(date)"; echo ": process still running after $duration seconds"

tmp=`mktemp XXXXX`
echo "thread apply all bt" > $tmp

for test_pid in $( pgrep -s 0 ); do
  cat <<EOF
==========

Attaching gdb to `ps o pid,cmd p $test_pid h`

==========
EOF

  gdb attach $test_pid < $tmp
done

rm $tmp

echo "Test failed and killing the stuck test process"

# Kill all processes in the test process session.
pkill -s 0

exit 1
