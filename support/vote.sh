#!/bin/bash

# This script should be used for calling a vote for a release candidate.
# In addition to publishing the source tarball to svn repo this script
# also deploys the corresponding jar to a staging Maven repo.

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

echo "${GREEN}Voting for mesos-${VERSION} candidate ${CANDIDATE}${NORMAL}"

read -p "Hit enter to continue ... "

MESOS_GIT_URL="https://git-wip-us.apache.org/repos/asf/mesos.git"

WORK_DIR=`mktemp -d /tmp/mesos-vote-XXXX`
atexit "rm -rf ${WORK_DIR}"

pushd ${WORK_DIR}

echo "${GREEN}Checking out ${TAG}${NORMAL}"

# First checkout the release tag.
git clone --depth 1 --branch ${TAG} ${MESOS_GIT_URL}

pushd mesos

# Ensure configure.ac has the correct version.
grep "\[mesos\], \[${VERSION}\]" configure.ac

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

# Loop here until the user enters a URL.
MAVEN_REPO=""
while [ -z ${MAVEN_REPO} ]; do
  read  -p "Please *close* the staging repository and provide its URL here: " MAVEN_REPO
done

# Build the distribution.
echo "${GREEN}Building the distribution ...${NORMAL}"
make -j3 dist

TARBALL=mesos-${VERSION}.tar.gz

echo "${GREEN}Signing the distribution ...${NORMAL}"

# Sign the tarball.
gpg --armor --output ${TARBALL}.asc --detach-sig ${TARBALL}

echo "${GREEN}Creating a MD5 checksum...${NORMAL}"

# Create MD5 checksum.
gpg --print-md MD5 ${TARBALL} > ${TARBALL}.md5

SVN_DEV_REPO="https://dist.apache.org/repos/dist/dev/mesos"
SVN_DEV_LOCAL="${WORK_DIR}/dev"

echo "${GREEN}Checking out svn dev repo ...${NORMAL}"

# Note '--depth=empty' ensures none of the existing files
# in the repo are checked out, saving time and space.
svn co --depth=empty ${SVN_DEV_REPO} ${SVN_DEV_LOCAL}

echo "${GREEN}Uploading the artifacts (the distribution," \
  "signature, and MD5) ...${NORMAL}"

RELEASE_DIRECTORY="${SVN_DEV_LOCAL}/${TAG}"
mkdir ${RELEASE_DIRECTORY}
mv ${TARBALL} ${TARBALL}.asc ${TARBALL}.md5 ${RELEASE_DIRECTORY}

popd # build
popd # mesos

pushd ${SVN_DEV_LOCAL}

svn add ${TAG}
svn commit -m "Adding mesos-${TAG}."

popd # ${SVN_DEV_LOCAL}

popd # ${WORK_DIR}

echo "${GREEN}Success! Now send the following VOTE email ...${NORMAL}"

# Create the email body template to be sent to {dev,user}@mesos.apache.org.
MESSAGE=$(cat <<__EOF__
To: dev@mesos.apache.org, user@mesos.apache.org
Subject: [VOTE] Release Apache Mesos ${VERSION} (rc${CANDIDATE})

Hi all,

Please vote on releasing the following candidate as Apache Mesos ${VERSION}.


${VERSION} includes the following:
--------------------------------------------------------------------------------
*****Announce major features here*****
*****Announce major bug fixes here*****

The CHANGELOG for the release is available at:
https://git-wip-us.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=${TAG}
--------------------------------------------------------------------------------

The candidate for Mesos ${VERSION} release is available at:
${SVN_DEV_REPO}/${TAG}/${TARBALL}

The tag to be voted on is ${TAG}:
https://git-wip-us.apache.org/repos/asf?p=mesos.git;a=commit;h=${TAG}

The MD5 checksum of the tarball can be found at:
${SVN_DEV_REPO}/${TAG}/${TARBALL}.md5

The signature of the tarball can be found at:
${SVN_DEV_REPO}/${TAG}/${TARBALL}.asc

The PGP key used to sign the release is here:
https://dist.apache.org/repos/dist/release/mesos/KEYS

The JAR is up in Maven in a staging repository here:
${MAVEN_REPO}

Please vote on releasing this package as Apache Mesos ${VERSION}!

The vote is open until `date -v+3d` and passes if a majority of at least 3 +1 PMC votes are cast.

[ ] +1 Release this package as Apache Mesos ${VERSION}
[ ] -1 Do not release this package because ...

Thanks,
__EOF__
)

echo "${MESSAGE}"

exit 0
