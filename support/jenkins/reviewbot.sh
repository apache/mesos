#!/usr/bin/env bash

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e
set -o pipefail

MESOS_DIR=$(git rev-parse --show-toplevel)

: "${USERNAME:?"Environment variable 'USERNAME' must be set to the username of the 'Mesos Reviewbot' Reviewboard account."}"
: "${PASSWORD:?"Environment variable 'PASSWORD' must be set to the password of the 'Mesos Reviewbot' Reviewboard account."}"

. "$MESOS_DIR"/support/atexit.sh

REVIEWS=$(mktemp -t reviewbot.XXXXXX)
atexit "rm ${REVIEWS}"

# Early exit if no reviews require validation.
"${MESOS_DIR}"/support/verify-reviews.py \
  -u "${USERNAME}" \
  -p "${PASSWORD}" \
  -r 1 \
  --skip-verify \
  --out-file "${REVIEWS}"

if [ ! -s "${REVIEWS}" ]; then
  echo "No reviews require verification"
  exit 0
fi

# Build the HEAD first to ensure that there are no errors prior to applying
# the review chain. We do not run tests at this stage.
export OS='ubuntu:16.04'
export BUILDTOOL='autotools'
export COMPILER='gcc'
export CONFIGURATION='--verbose --disable-libtool-wrappers --disable-parallel-test-execution'
export ENVIRONMENT='GLOG_v=1 MESOS_VERBOSE=1 GTEST_FILTER='
"${MESOS_DIR}"/support/jenkins/buildbot.sh

# NOTE: The script sets up its own environment.
"${MESOS_DIR}"/support/verify-reviews.py \
  -u "${USERNAME}" \
  -p "${PASSWORD}" \
  -r 1
