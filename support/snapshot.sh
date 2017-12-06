#!/usr/bin/env bash

# This script should be used to publish a snapshot JAR to Apache Maven
# repo from the current HEAD.

set -e

# Use 'atexit' for cleanup.
. $(dirname ${0})/atexit.sh

# Use colors for errors.
. $(dirname ${0})/colors.sh

test ${#} -eq 1 || \
  { echo "Usage: `basename ${0}` [version]"; exit 1; }

VERSION=${1}
TAG="${VERSION}-SNAPSHOT"

echo "${GREEN}Deploying ${TAG}${NORMAL}"

read -p "Hit enter to continue ... "

WORK_DIR=`mktemp -d /tmp/mesos-tag-XXXX`
atexit "rm -rf ${WORK_DIR}"

# Get the absolute path of the local git clone.
MESOS_GIT_LOCAL=$(cd "$(dirname $0)"/..; pwd)

pushd ${WORK_DIR}

# Make a shallow clone from the local git repository.
git clone --shared ${MESOS_GIT_LOCAL} mesos

pushd mesos

# Ensure configure.ac has the correct version.
echo "Confirming that configure.ac contains ${VERSION}"
grep "\[mesos\], \[${VERSION}\]" configure.ac

echo "${GREEN}Updating configure.ac to include 'SNAPSHOT'.${NORMAL}"
sed -i '' "s/\[mesos\], \[.*\]/[mesos], [${TAG}]/" configure.ac

# Build mesos.
./bootstrap
mkdir build
pushd build
../configure --disable-optimize

# First build the protobuf compiler.
# TODO(vinod): This is short term fix for MESOS-959.
pushd 3rdparty
make -j3
popd

# Build and deploy the jar.
make -j3 maven-install
mvn deploy -f src/java/mesos.pom

echo "${GREEN}Successfully deployed the jar to maven snapshot repository ...${NORMAL}"

popd # build

popd # mesos
popd # ${WORK_DIR}

echo "${GREEN}Success!${NORMAL}"

exit 0
