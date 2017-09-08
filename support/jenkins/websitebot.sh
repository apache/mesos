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

# This script is executed by ASF CI to automatically publish the website
# whenever there is a new commit in the mesos repo.
set -e
set -o pipefail

: "${WORKSPACE:?"Environment variable 'WORKSPACE' must be set"}"

export MESOS_DIR="$WORKSPACE/mesos"
export MESOS_SITE_DIR="$WORKSPACE/mesos-site"

pushd "$MESOS_DIR"

if [ "$(git status --porcelain | wc -l)" -ne 0 ]; then
  echo "This script is not intended to run on a live checkout, but the source
        tree contains untracked files or uncommitted changes."

  exit 1
fi

MESOS_HEAD_SHA=$(git rev-parse --verify --short HEAD)

./support/mesos-website.sh

popd # $MESOS_DIR

# Commit the updated changes to the site repo for website update.
pushd "$MESOS_SITE_DIR"

git add .

if ! git diff --cached --quiet; then
  git commit -m "Updated the website built from mesos SHA: $MESOS_HEAD_SHA."
  git push origin HEAD:refs/heads/asf-site
else
  echo "No changes to website detected"
fi

popd # $MESOS_SITE_DIR
