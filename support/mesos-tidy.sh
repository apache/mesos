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

MESOS_DIR=$(git rev-parse --show-toplevel)

DEFAULT_CHECKS=

DEFAULT_CHECKS=${DEFAULT_CHECKS},'clang-diagnostic-*'

DEFAULT_CHECKS=${DEFAULT_CHECKS},'clang-analyzer-*'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-clang-analyzer-'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-clang-analyzer-core.CallAndMessage'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-clang-analyzer-core.NonNullParamChecker'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-clang-analyzer-core.NullDereference'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-clang-analyzer-cplusplus.NewDelete'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-clang-analyzer-optin.cplusplus.VirtualCall'

DEFAULT_CHECKS=${DEFAULT_CHECKS},'google-*'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-google-build-using-namespace'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-google-default-arguments'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-google-explicit-constructor'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-google-global-names-in-headers'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-google-readability-namespace-comments'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-google-runtime-int'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-google-runtime-member-string-references'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-google-readability-casting'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-google-runtime-references'

DEFAULT_CHECKS=${DEFAULT_CHECKS},'mesos-*'
DEFAULT_CHECKS=${DEFAULT_CHECKS},'-mesos-this-capture'

DEFAULT_CHECKS=${DEFAULT_CHECKS},'readability-redundant-string-cstr'

# Configure how checks are run. These variables can be overridden by setting the
# respective environment variables before invoking this script.
# TODO(bbannier): Enable more upstream checks by default, e.g., from the Google set.
CHECKS=${CHECKS:-${DEFAULT_CHECKS}}

# Check for unstaged or uncommitted changes.
if ! $(git diff-index --quiet HEAD --); then
  echo 'Please commit or stash any changes before running `mesos-tidy`.'
  exit 1
fi

# Pull the `mesos-tidy` image from Docker Hub.
docker pull mesos/mesos-tidy

# Execute the container.
docker run \
  --rm \
  -v "${MESOS_DIR}":/SRC:Z \
  -e CHECKS="${CHECKS}" \
  -e CMAKE_ARGS="${CMAKE_ARGS}" \
  mesos/mesos-tidy
