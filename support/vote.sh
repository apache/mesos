#!/usr/bin/env bash

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

if ! git rev-parse "$TAG" > /dev/null 2>&1; then
  echo "Tag $TAG doesn't exist. Please create one using:"
  echo "  git tag -a $TAG -m \"Tagging Mesos $TAG.\""
  exit 1
fi

if [ "$(git cat-file -t $TAG)" != "tag" ]; then
  echo "Tag $TAG is not annotated. First delete the existing tag using:"
  echo "  git tag -d $TAG"
  echo "Then create an annotated tag using:"
  echo "  git tag -a $TAG -m \"Tagging Mesos $TAG.\""
  exit 1;
fi

# Releases are signed with `sha512sum` which is installed as
# `gsha512sum` from Homebrew's `coreutils` package.
echo "Checking for sha512sum or gsha512sum"
SHA512SUM=$(command -v sha512sum || command -v gsha512sum)

echo "${GREEN}Tagging and Voting for mesos-${VERSION} candidate ${CANDIDATE}${NORMAL}"

read -p "Hit enter to continue ... "

MESOS_GIT_URL="https://gitbox.apache.org/repos/asf/mesos.git"

# Get the absolute path of the local git clone.
MESOS_GIT_LOCAL=$(cd "$(dirname $0)"/..; pwd)

WORK_DIR=`mktemp -d /tmp/mesos-tag-vote-XXXX`
atexit "rm -rf ${WORK_DIR}"

pushd ${WORK_DIR}

echo "${GREEN}Checking out ${TAG}${NORMAL}"

# Make a shallow clone from the local git repository.
git clone --shared ${MESOS_GIT_LOCAL} --branch ${TAG} mesos

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
pushd 3rdparty
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

echo "${GREEN}Pushing the git tag to the repository...${NORMAL}"
git push ${MESOS_GIT_URL} refs/tags/${TAG}

# Build the distribution.
echo "${GREEN}Building the distribution ...${NORMAL}"
make -j3 dist

TARBALL=mesos-${VERSION}.tar.gz

echo "${GREEN}Signing the distribution ...${NORMAL}"

# Sign the tarball.
gpg --armor --output ${TARBALL}.asc --detach-sig ${TARBALL}

echo "${GREEN}Creating a SHA512 checksum ...${NORMAL}"

# Create SHA512 checksum.
"${SHA512SUM}" ${TARBALL} > ${TARBALL}.sha512

SVN_DEV_REPO="https://dist.apache.org/repos/dist/dev/mesos"
SVN_DEV_LOCAL="${WORK_DIR}/dev"

echo "${GREEN}Checking out svn dev repo ...${NORMAL}"

# Note '--depth=empty' ensures none of the existing files
# in the repo are checked out, saving time and space.
svn co --depth=empty ${SVN_DEV_REPO} ${SVN_DEV_LOCAL}

echo "${GREEN}Uploading the artifacts (the distribution," \
  "signature, and checksum) ...${NORMAL}"

RELEASE_DIRECTORY="${SVN_DEV_LOCAL}/${TAG}"
mkdir ${RELEASE_DIRECTORY}
mv ${TARBALL} ${TARBALL}.asc ${TARBALL}.sha512 ${RELEASE_DIRECTORY}

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
https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=${TAG}
--------------------------------------------------------------------------------

The candidate for Mesos ${VERSION} release is available at:
${SVN_DEV_REPO}/${TAG}/${TARBALL}

The tag to be voted on is ${TAG}:
https://gitbox.apache.org/repos/asf?p=mesos.git;a=commit;h=${TAG}

The SHA512 checksum of the tarball can be found at:
${SVN_DEV_REPO}/${TAG}/${TARBALL}.sha512

The signature of the tarball can be found at:
${SVN_DEV_REPO}/${TAG}/${TARBALL}.asc

The PGP key used to sign the release is here:
https://dist.apache.org/repos/dist/release/mesos/KEYS

The JAR is in a staging repository here:
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
