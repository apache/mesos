#!/usr/bin/env bash

# This script should be used *after* a successful vote to publish a
# release. In addition to publishing the source tarball to svn repo
# this script also publishes the corresponding jar to Maven.

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

echo "${GREEN}Releasing mesos-${TAG} as mesos-${VERSION}${NORMAL}"

read -p "Hit enter to continue ... "

WORK_DIR=`mktemp -d /tmp/mesos-release-XXXX`
atexit "rm -rf ${WORK_DIR}"

pushd ${WORK_DIR}

SVN_DEV_REPO="https://dist.apache.org/repos/dist/dev/mesos"

echo "${GREEN}Downloading the artifacts from the dev repo ...${NORMAL}"
svn export ${SVN_DEV_REPO}/${TAG}

SVN_RELEASE_REPO="https://dist.apache.org/repos/dist/release/mesos"
SVN_RELEASE_LOCAL="${WORK_DIR}/release"

echo "${GREEN}Checking out svn release repo ...${NORMAL}"

# Note '--depth=empty' ensures none of the existing files
# in the repo are checked out, saving time and space.
svn co --depth=empty ${SVN_RELEASE_REPO} ${SVN_RELEASE_LOCAL}

echo "${GREEN}Uploading the artifacts (the distribution," \
  "signature, and checksum) to the release repo ${NORMAL}"

mv ${TAG} ${SVN_RELEASE_LOCAL}/${VERSION}

pushd ${SVN_RELEASE_LOCAL}

svn add ${VERSION}
svn commit -m "Adding mesos-${VERSION}."

popd # ${SVN_RELEASE_LOCAL}

popd # ${WORK_DIR}

echo "${GREEN}Tagging ${TAG} as ${VERSION} ${NORMAL}"

git tag -a ${VERSION} ${TAG} -m "Tagging Mesos ${VERSION}"

echo "${GREEN}Pushing the git tag to the repository...${NORMAL}"

MESOS_GIT_URL="https://gitbox.apache.org/repos/asf/mesos.git"

git push ${MESOS_GIT_URL} ${VERSION}

echo "${GREEN}Successfully published artifacts to svn release repo ...${NORMAL}"

echo "${GREEN}Please *release* the staging maven repository that contains the mesos jar ...${NORMAL}"

input=""
while [ ! ${input:-n} = "y" ]; do
  read -p "Have you released the maven repository? (y/n): " input
  [ ${input:-n} = "y" ] || echo "Please release the staging maven repository before continuing"
done

echo "${GREEN}Success! Now send the following RESULT VOTE email ...${NORMAL}"

# Create the email body template to be sent to {dev,user}@mesos.apache.org.
MESSAGE=$(cat <<__EOF__
To: dev@mesos.apache.org, user@mesos.apache.org
Subject: [RESULT][VOTE] Release Apache Mesos ${VERSION} (rc${CANDIDATE})

Hi all,

The vote for Mesos ${VERSION} (rc${CANDIDATE}) has passed with the
following votes.

+1 (Binding)
------------------------------
***
***
***

+1 (Non-binding)
------------------------------
***
***
***

There were no 0 or -1 votes.

Please find the release at:
${SVN_RELEASE_REPO}/${VERSION}

It is recommended to use a mirror to download the release:
http://www.apache.org/dyn/closer.cgi

The CHANGELOG for the release is available at:
https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=${VERSION}

The mesos-${VERSION}.jar has been released to:
https://repository.apache.org

The website (http://mesos.apache.org) will be updated shortly to reflect this release.

Thanks,
__EOF__
)

echo "${MESSAGE}"

exit 0
