#!/usr/bin/env bash

set -e
set -o pipefail

# Check for unstaged or uncommitted changes.
if ! $(git diff-index --quiet HEAD --); then
  echo 'Please commit or stash any changes before running `dist`.'
  exit 1
fi

VERSION=$1
if [ -z "${VERSION}" ]; then
  echo "Specify a version number as first argument"
  exit 1
fi

ORIGIN=$PWD
MESOS_DIR=$(git rev-parse --show-toplevel)

WORKDIR=$(mktemp -d)
trap 'rm -rf ${WORKDIR}' EXIT

pushd "${WORKDIR}" || exit 1

git clone "${MESOS_DIR}"
mkdir build
pushd build

cmake ../$(basename "${MESOS_DIR}")
cmake --build . --target package_source -j "$(nproc)"
cp mesos-"${VERSION}".tar.gz "${ORIGIN}"

popd
popd

echo "Successfully created ${ORIGIN}/mesos-${VERSION}.tar.gz"
