#!/bin/bash

# This script should be used to publish a local tag of the release
# candidate to the Apache mesos repo. In addition this script also
# publishes the corresponding mesos jar to a Maven repo.

set -e

# Use 'atexit' for cleanup.
. $(dirname ${0})/atexit.sh

# Use colors for errors.
. $(dirname ${0})/colors.sh

test ${#} -eq 2 || \
  { echo "Usage: `basename ${0}` [version] [candidate]"; exit 1; }

VERSION=${1}
CANDIDATE=${2}
TAG="${VERSION}-rc${CANDIDATE}"

echo "${GREEN}Tagging ${TAG}${NORMAL}"

read -p "Hit enter to continue ... "

WORK_DIR=`mktemp -d /tmp/mesos-tag-XXXX`
atexit "rm -rf ${WORK_DIR}"

# Get the absolulte path of the local git clone.
MESOS_GIT_LOCAL=$(cd "$(dirname $0)"/..; pwd)

pushd ${WORK_DIR}

# Make a shallow clone from the local git repository.
git clone --shared ${MESOS_GIT_LOCAL} --branch ${TAG} mesos

pushd mesos

# Ensure configure.ac has the correct version.
echo "Confirming that configure.ac contains ${VERSION}"
grep "\[mesos\], \[${VERSION}\]" configure.ac

echo "${GREEN}Updating configure.ac to include the release candidate.${NORMAL}"
sed -i '' "s/\[mesos\], \[.*\]/[mesos], [${TAG}]/" configure.ac

# Build mesos.
./bootstrap
mkdir build
pushd build
../configure --disable-optimize

# First build the protobuf compiler.
# TODO(vinod): This is short term fix for MESOS-959.
pushd 3rdparty/libprocess/3rdparty
make -j3
popd

# Build and deploy the jar.
make -j3 maven-install
mvn deploy -f src/java/mesos.pom

echo "${GREEN}Successfully deployed the jar to staging maven repository ...${NORMAL}"

read  -p "Please *close* and *release* the staging repository and hit enter to continue ..."

input=""
while [ ! ${input:-n} = "y" ]; do
  read -p "Have you released the maven repository? (y/n): " input
  [ ${input:-n} = "y" ] || echo "Please release the staging maven repository before continuing"
done

popd # build

echo "${GREEN}Pushing the git tag to the repository...${NORMAL}"

MESOS_GIT_URL="https://git-wip-us.apache.org/repos/asf/mesos.git"

git push ${MESOS_GIT_URL} refs/tags/${TAG}

popd # mesos
popd # ${WORK_DIR}

echo "${GREEN}Success!${NORMAL}"

exit 0
