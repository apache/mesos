#!/bin/bash

# Use colors for errors.
. $(dirname ${0})/colors.sh

test ${#} -eq 2 || \
  { echo "Usage: `basename ${0}` [version] [candidate]"; exit 1; }

# TODO(benh): Figure out a way to get version number and release
# candidate automagically.
VERSION=${1}
CANDIDATE=${2}

echo "${GREEN}Releasing mesos-${VERSION} candidate ${CANDIDATE}${NORMAL}"

read -p "Hit enter to continue ... "

make distcheck || \
  { echo "${RED}Failed to check the distribution${NORMAL}"; exit 1; }

TARBALL=mesos-${VERSION}.tar.gz

echo "${GREEN}Now let's sign the distribution ...${NORMAL}"

# Sign the tarball.
gpg --armor --output ${TARBALL}.asc --detach-sig ${TARBALL} || \
  { echo "${RED}Failed to sign the distribution${NORMAL}"; exit 1; }

echo "${GREEN}And let's create a MD5 checksum...${NORMAL}"

# Create MD5 checksum.
gpg --print-md MD5 ${TARBALL} > ${TARBALL}.md5 || \
  { echo "${RED}Failed to create MD5 for distribution${NORMAL}"; exit 1; }

DIRECTORY=public_html/mesos-${VERSION}-RC${CANDIDATE}

echo "${GREEN}Now let's upload our artifacts (the distribution," \
  "signature, and MD5) ...${NORMAL}"

ssh people.apache.org "mkdir -p ${DIRECTORY}" || \
  { echo "${RED}Failed to create remote directory${NORMAL}"; exit 1; }

{ scp ${TARBALL} people.apache.org:${DIRECTORY}/ && \
  scp ${TARBALL}.asc people.apache.org:${DIRECTORY}/ && \
  scp ${TARBALL}.md5 people.apache.org:${DIRECTORY}/; } || \
  { echo "${RED}Failed to copy distribution artifacts${NORMAL}"; exit 1; }

echo "${GREEN}Now let's make the artifacts world readable ...${NORMAL}"

{ ssh people.apache.org "chmod a+r ${DIRECTORY}/${TARBALL}" && \
  ssh people.apache.org "chmod a+r ${DIRECTORY}/${TARBALL}.asc" && \
  ssh people.apache.org "chmod a+r ${DIRECTORY}/${TARBALL}.md5"; } || \
  { echo "${RED}Failed to change permissions of artifacts${NORMAL}";
    exit 1; }

echo "${GREEN}Now we'll create a git tag ...${NORMAL}"

MESSAGE="Tag for ${VERSION}-rc${CANDIDATE}."
TAG="${VERSION}-rc${CANDIDATE}"
git tag -m "${MESSAGE}" ${TAG} || \
  { echo "${RED}Failed to create git tag${NORMAL}"; exit 1; }

echo "${GREEN}Finally, we'll push the git tag to the repository...${NORMAL}"

REPOSITORY="https://git-wip-us.apache.org/repos/asf/mesos.git"
git push ${REPOSITORY} ${TAG} || \
  { echo "${RED}Failed to push git tag to the repo${NORMAL}"; exit 1; }

exit 0
