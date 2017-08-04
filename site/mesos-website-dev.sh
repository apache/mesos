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
# TODO(vinod): Simplify the development workflow by getting rid of `docker`
# or consolidating with the publish workflow in `support/mesos-website.sh`.
# See MESOS-7860 for details.

set -e
set -o pipefail

MESOS_DIR=$(git rev-parse --show-toplevel)

pushd "$MESOS_DIR"

TAG=mesos/website-`date +%s`-$RANDOM

docker build -t $TAG site

trap 'docker rmi $TAG' EXIT

docker run \
  -it \
  --rm \
  -p 4567:4567 \
  -p 35729:35729 \
  -v $MESOS_DIR:/mesos:Z \
  $TAG

popd # $MESOS_DIR
