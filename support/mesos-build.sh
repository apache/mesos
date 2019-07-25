#!/usr/bin/env bash

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at

#     http://www.apache.org/licenses/LICENSE-2.0

# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e
set -o pipefail

: "${OS:?"Environment variable 'OS' must be set"}"
: "${BUILDTOOL:?"Environment variable 'BUILDTOOL' must be set"}"
: "${COMPILER:?"Environment variable 'COMPILER' must be set"}"
: "${CONFIGURATION:?"Environment variable 'CONFIGURATION' must be set"}"
: "${ENVIRONMENT:?"Environment variable 'ENVIRONMENT' must be set"}"
: "${JOBS:=$(nproc)}"

MESOS_DIR=$(git rev-parse --show-toplevel)

# Check for unstaged or uncommitted changes.
if ! $(git diff-index --quiet HEAD --); then
  echo 'Please commit or stash any changes before running `mesos-build`.'
  exit 1
fi

# NOTE: We chmod the directory here so that the docker containter can
# copy out the test report xml files from the container file system.
chmod 777 "${MESOS_DIR}"

# Update docker image
docker pull "mesos/mesos-build:${OS//:/-}"

docker run \
  --rm \
  -v "${MESOS_DIR}":/SRC:Z \
  -e BUILDTOOL="${BUILDTOOL}" \
  -e COMPILER="${COMPILER}" \
  -e CONFIGURATION="${CONFIGURATION}" \
  -e ENVIRONMENT="${ENVIRONMENT}" \
  -e JOBS="${JOBS}" \
  "mesos/mesos-build:${OS//:/-}"
