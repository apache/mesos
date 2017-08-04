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

# This is a wrapper for building Mesos website locally.
set -e
set -o pipefail

MESOS_DIR=$(git rev-parse --show-toplevel)

: "${MESOS_SITE_DIR:?"Environment variable 'MESOS_SITE_DIR' must be set"}"

pushd "$MESOS_DIR"

TAG=mesos/website-`date +%s`-$RANDOM

docker build --no-cache=true -t $TAG support/mesos-website

trap 'docker rmi $TAG' EXIT

# NOTE: We set `LOCAL_USER_ID` environment variable to enable running the
# container process with the same UID as the user running this script. This
# ensures that any writes to the mounted volumes will have the same permissions
# as the user running the script, making it easy to do cleanups; otherwise
# any writes will have permissions of UID 0 by default on Linux.

docker run \
  --rm \
  -e LOCAL_USER_ID="$(id -u "$USER")" \
  -v "$MESOS_DIR":/mesos:Z \
  -v "$MESOS_SITE_DIR/content":/mesos/site/publish:Z \
  $TAG

popd # $MESOS_DIR
