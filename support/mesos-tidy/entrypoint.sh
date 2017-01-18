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

SRCDIR=/tmp/SRC

# Prepare sources
git clone --depth 1 file:///SRC "${SRCDIR}"
(cd "${SRCDIR}" && ./bootstrap)

# Configure sources
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      ${CMAKE_ARGS} \
      "${SRCDIR}"

# Build the external dependencies.
# TODO(mpark): Use an external dependencies target once MESOS-6924 is resolved.
cmake --build 3rdparty --target boost-1.53.0 -- -j $(nproc)
cmake --build 3rdparty --target elfio-3.2 -- -j $(nproc)
cmake --build 3rdparty --target glog-0.3.3 -- -j $(nproc)
cmake --build 3rdparty --target gmock-1.7.0 -- -j $(nproc)
cmake --build 3rdparty --target http_parser-2.6.2 -- -j $(nproc)

# TODO(mpark): The `|| true` is a hack to try both `libev` and `libevent` and
#              use whichever one happens to be configured. This would also go
#              away with MESOS-6924.
cmake --build 3rdparty --target libev-4.22 -- -j $(nproc) || true
cmake --build 3rdparty --target libevent-2.1.5-beta -- -j $(nproc) || true

cmake --build 3rdparty --target leveldb-1.4 -- -j $(nproc)
cmake --build 3rdparty --target nvml-352.79 -- -j $(nproc)
cmake --build 3rdparty --target picojson-1.3.0 -- -j $(nproc)
cmake --build 3rdparty --target protobuf-2.6.1 -- -j $(nproc)
cmake --build 3rdparty --target zookeeper-3.4.8 -- -j $(nproc)

# Generate the protobuf definitions.
# TODO(mpark): Use a protobuf generation target once MESOS-6925 is resolved.
cmake --build . --target mesos-protobufs -- -j $(nproc)

# TODO(bbannier): Use a less restrictive `grep` pattern and `header-filter`
# once MESOS-6115 is fixed.
cat compile_commands.json \
  | jq '.[].file' \
  | sed 's/"//g' \
  | sed 's/^\ //g' \
  | grep "^${SRCDIR}/.*\.cpp$" \
  | parallel -j $(nproc) clang-tidy -p "${PWD}" \
      -extra-arg=-Wno-unknown-warning-option \
      -extra-arg=-Wno-unused-command-line-argument \
      -header-filter="^${SRCDIR}/.*\.hpp$" -checks="${CHECKS}" \
  1> clang-tidy.log 2> /dev/null

# Propagate any errors.
if test -s clang-tidy.log; then
  cat clang-tidy.log
  exit 1
else
  echo "No mesos-tidy violations found."
fi
