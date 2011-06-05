#! /usr/bin/env sh

NUM_SLAVES=$1

if [ $NUM_SLAVES ]; then
  ./mesos-ec2 -k andyk -i ~/.ec2/andyk.pem -s $NUM_SLAVES launch mesos-torque-fw -d git -b torque
else
  echo "error, specify num nodes"
  exit
fi

