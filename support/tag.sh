#!/bin/bash

# This script should be used to tag the current HEAD as a release
# candidate and publish the tag to the mesos repo. In addition this
# script also publishes the corresponding mesos jar to a Maven repo.

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

echo "${GREEN}Tagging the HEAD as ${TAG}${NORMAL}"

read -p "Hit enter to continue ... "

WORK_DIR=`mktemp -d /tmp/mesos-tag-XXXX`
atexit "rm -rf ${WORK_DIR}"

# Get the absolulte path of the local git clone.
MESOS_GIT_LOCAL=$(cd "$(dirname $0)"/..; pwd)

pushd ${WORK_DIR}

# Make a shallow clone from the local git repository.
git clone --shared ${MESOS_GIT_LOCAL} mesos

pushd mesos

# Ensure configure.ac has the correct version.
grep "\[mesos\], \[${VERSION}\]" configure.ac

echo "${GREEN}Updating configure.ac to include the release candidate.${NORMAL}"
sed -i '' "s/\[mesos\], \[.*\]/[mesos], [${TAG}]/" configure.ac

# Build mesos.
./bootstrap
mkdir build
pushd build
../configure --disable-optimize
make -j3 check

# Build the distribution.
# TODO(vinod): Also upload the distribution to svn dev repo?
# Currently this is tricky because support/vote.sh uploads the
# distribution in svn dev repo under mesos-${TAG} directory.
make -j3 dist

# Build and deploy the jar.
make maven-install
mvn deploy -f src/java/mesos.pom

echo "${GREEN}Successfully deployed the jar to staging maven repository ...${NORMAL}"

read  -p "Please *close* and *release* the staging repository and hit enter to continue ..."

input=""
while [ ! ${input:-n} = "y" ]; do
  read -p "Have you released the maven repository? (y/n): " input
  [ ${input:-n} = "y" ] || echo "Please release the staging maven repository before continuing"
done

popd # build

echo "${GREEN}Creating ${TAG} tag at HEAD...${NORMAL}"

git tag ${TAG} || echo "Tag ${TAG} already exists"

echo "${GREEN}Pushing the git tag to the repository...${NORMAL}"

MESOS_GIT_URL="https://git-wip-us.apache.org/repos/asf/mesos.git"

git push ${MESOS_GIT_URL} ${TAG}

popd # mesos
popd # ${WORK_DIR}

echo "${GREEN}Success!${NORMAL}"

exit 0
