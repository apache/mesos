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

cd "${SRCDIR}"

export GTEST_OUTPUT=xml:/SRC/
export DISTCHECK_CONFIGURE_FLAGS=${CONFIGURATION}
export ${ENVIRONMENT}

case ${COMPILER} in
  gcc)
    export CC=gcc
    export CXX=g++
    ;;
  clang)
    export CC=clang
    export CXX=clang++
    ;;
  *)
    echo "Unknown compiler ${COMPILER}"
    exit 1
    ;;
esac

case ${BUILDTOOL} in
  autotools)
    ./bootstrap
    mkdir build && cd build
    ../configure ${CONFIGURATION}
    make -j "${JOBS}" distcheck 2>&1
    ;;
  cmake)
    # Transform autotools-like parameters to cmake-like.
    # Remove "'".
    CONFIGURATION=${CONFIGURATION//\'/""}
    # Replace "-" with "_".
    CONFIGURATION=${CONFIGURATION//-/"_"}
    # Replace "__" with "-D".
    CONFIGURATION=${CONFIGURATION//__/"-D"}
    # To Upper Case.
    CONFIGURATION=${CONFIGURATION^^}

    # Add "=1" suffix to each variable.
    IFS=' ' read -r  -a array <<< "${CONFIGURATION}"

    CONFIGURATION=""
    for element in "${array[@]}"
    do
      CONFIGURATION="${CONFIGURATION} $element=1"
    done

    mkdir build && cd build
    cmake ${CONFIGURATION} ..
    make -j "${JOBS}" check 2>&1
    ;;
  *)
    echo "Unknown build tool ${BUILDTOOL}"
    exit 1
    ;;
esac
