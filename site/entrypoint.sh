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
bundle install

# We do not create a temp user for MacOS because the host volumes are always
# mapped to UID 1000 and GID 1000 inside the container. And any writes by the
# root user inside the container to the mounted host volume will end up with
# the original UID and GID of the host user.
# TODO(vinod): On Linux, create a temp user and run the below script as that
# user to avoid doing any writes as root user on host volumes.
/mesos/site/build.sh
