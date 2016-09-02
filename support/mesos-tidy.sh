#!/bin/bash

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

# Configure how checks are run. These variables can be overriden by setting the
# respective environment variables before invoking this script.
# TODO(bbannier): Enable more upstream checks by default, e.g., from the Google set.
CHECKS=${CHECKS:-'-*,mesos-*'}

# By default perform an optimized build since it appears to finish
# slightly faster. Note that this has no effect on the static analysis.
CONFIGURE_FLAGS=${CONFIGURE_FLAGS:-'--enable-optimize'}

MESOS_DIRECTORY=$(cd "$(dirname "$0")/.." && pwd)

# Check for unstaged or uncommitted changes.
if ! $(git diff-index --quiet HEAD --); then
  echo 'Please commit or stash any changes before running `mesos-tidy`.'
  exit 1
fi

# Execute the container.
docker run \
  --rm \
  -v "${MESOS_DIRECTORY}":/SRC \
  -e CHECKS="${CHECKS}" \
  -e CONFIGURE_FLAGS="${CONFIGURE_FLAGS}" \
  mesosphere/mesos-tidy || exit 1
