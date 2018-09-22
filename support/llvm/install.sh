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

: "${VERSION:="2018-03-08"}"

LLVM_DIR="$(git rev-parse --git-common-dir)/llvm"
INSTALL_DIR="${LLVM_DIR}/${VERSION}"

mkdir -p "${LLVM_DIR}"

function activate() {
  ln -nsf "${VERSION}" "${LLVM_DIR}/active"
  echo "'${LLVM_DIR}/active' now points at '${VERSION}'."
}

if [ -d "${INSTALL_DIR}" ]; then
  echo "Mesos LLVM tools ${VERSION} are already installed at '${INSTALL_DIR}'."

  activate
  exit 1
fi

mkdir -p "${INSTALL_DIR}"

OS="$(uname -s)"

BINTRAY_URL="https://apache.bintray.com"

case "${OS}" in
  Linux)
    EXT="linux.tar.gz"
    ;;
  Darwin)
    VER="$(sw_vers -productVersion)"
    case "${VER}" in
      10.11.*)
        EXT="el_capitan.bottle.tar.gz"
        ;;
      10.12.*)
        EXT="sierra.bottle.tar.gz"
        ;;
      10.13.*)
        EXT="high_sierra.bottle.tar.gz"
        ;;
      *)
        echo "Mesos LLVM tools are currently not available for Mac OS X ${VER}."
        exit 2
        ;;
    esac
    ;;
  *)
    echo "Mesos LLVM tools are currently not available for ${OS}."
    exit 2
    ;;
esac

for TOOL in "mesos-format" "mesos-tidy"
do
  URL="${BINTRAY_URL}/mesos/${TOOL}-${VERSION}.${EXT}"
  curl -sSL "${URL}" | tar -xz --strip-components=2 -C "${INSTALL_DIR}"
  if [ $? -eq 0 ]; then
    echo "Installed '${TOOL}' in '${INSTALL_DIR}'."
  fi
done

activate
