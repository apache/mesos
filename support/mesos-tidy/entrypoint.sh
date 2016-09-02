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

SRCDIR=/tmp/SRC

# Prepare sources
git clone --depth 1 file:///SRC "${SRCDIR}" || exit 1
(cd ${SRCDIR} && ./bootstrap) || exit 1

# Configure sources
${SRCDIR}/configure ${CONFIGURE_FLAGS} || exit 1

# Build sources
bear make -j $(nproc) tests || exit 1

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
