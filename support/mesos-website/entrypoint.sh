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

# This is a wrapper script for building Mesos website as a non-root user.
set -e
set -o pipefail

# This needs to be run under `root` user for `bundle exec rake` to
# work properly. See MESOS-7859.
pushd site
bundle install
popd # site

# Create a local user account.
useradd -u $LOCAL_USER_ID -s /bin/bash -m tempuser

# Build mesos and the website as the new user.
su -c support/mesos-website/build.sh tempuser
