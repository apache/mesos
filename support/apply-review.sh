#!/usr/bin/env bash

if [ ! -f support/apply-reviews.py ]; then
  echo 'Please run this script from the root of Mesos source directory.'
  exit 1
fi

exec python support/apply-reviews.py "$@"
