#!/usr/bin/env bash

set -e
set -o pipefail

ORIGIN=${PWD}

VERSION=$1
if [ -z "${VERSION}" ]; then
  echo "Specify a version number as first argument"
  exit 1
fi

WORKDIR=$(mktemp -d)
trap 'rm -rf ${WORKDIR}' EXIT

pushd "${WORKDIR}" || exit 1

tar xf "${ORIGIN}"/mesos-"${VERSION}".tar.gz
pushd mesos-"${VERSION}"
mkdir build
pushd  build

# TODO(bbannier): Also check `install` target once the CMake build supports it.
cmake .. ${DISTCHECK_CMAKE_FLAGS}
cmake --build . --target check -j "$(nproc)"

popd
popd

popd
