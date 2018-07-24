#!/bin/sh
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

# This script can be used to detect C++ source files present in the
# tree, but not mentioned in any `CMakeLists.txt`.


set -e
set -o pipefail

readonly MESOS_DIR="$(git rev-parse --show-toplevel)"

. "$MESOS_DIR"/support/atexit.sh


SOURCE_DIRS="$MESOS_DIR/src $MESOS_DIR/3rdparty"


# Get a list of all source files in the tree.
#
# We rely on the fact that in Mesos source files end in `.cpp`. We
# only care about files known to git.
readonly FILES=$(mktemp)
atexit "rm $FILES"
for file in $(cd "$MESOS_DIR" && git ls-files "$MESOS_DIR" | grep '.*\.cpp'); do
  realpath "$MESOS_DIR/$file"
done | sort > "$FILES"


# Get a list of all source files in the cmake build.
#
# Since Mesos add source files to the cmake build conditionally, even
# cmake does not know the full list of all possible source files in
# our current setup. As a heuristic we search the cmake build for
# likely source file names.
readonly CMAKE_FILES=$(mktemp)
atexit "rm $CMAKE_FILES"

for cm in $(find $SOURCE_DIRS -name CMakeLists.txt); do
  base="$(dirname "$cm")"

  for file in $(grep -o '\(\S\)*\w\+\.cpp' "$cm"); do
    # We ignore weird filename-like patterns which do
    # not correspond to a path.
    realpath "$base/$file" 2> /dev/null || true
  done
done | sort > "$CMAKE_FILES"


MISSING_FILES=$(comm -23 "$FILES" "$CMAKE_FILES")

if [ -n "$MISSING_FILES" ]; then
  (>&2 \
    printf "%s\\n\\n%s\\n" \
      "The following files do not appear in any CMakeLists.txt:" \
      "$MISSING_FILES" \
  )

  exit 1
fi
