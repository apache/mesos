#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail -o verbose

# This script builds RPM package for Mesos using Docker.

CENTOS_DIR="$(cd "$(dirname "$0")"; pwd -P)"
SOURCE_DIR="$(cd "${CENTOS_DIR}/../../.."; pwd -P)"

CENTOS_DISTRO=${CENTOS_DISTRO:-"7"}
DOCKER_IMAGE=${DOCKER_IMAGE:-"mesos/mesos-centos${CENTOS_DISTRO}-rpmbuild"}
DOCKER_IMAGE_TAG=${DOCKER_IMAGE_TAG:-"latest"}

if ! [ -x "$(command -v docker)" ]; then
  echo 'Error: docker is not installed.' >&2
  exit 1
fi

MESOS_SHA=${MESOS_SHA:-$(git rev-parse HEAD)}
MESOS_TAG_OR_SHA=$(git describe --exact-match "${MESOS_SHA}" 2>/dev/null || echo "${MESOS_SHA}")

if [[ "${MESOS_TAG_OR_SHA}" != "${MESOS_SHA}" ]]; then
  # HEAD is also a tag.
  MESOS_TAG="${MESOS_TAG_OR_SHA}"
else
  MESOS_TAG=""
fi

echo "MESOS_SHA=${MESOS_SHA}"
echo "MESOS_TAG=${MESOS_TAG}"

rm -rf "${SOURCE_DIR}/centos${CENTOS_DISTRO}"

CENTOS_DISTRO="${CENTOS_DISTRO}" \
DOCKER_IMAGE="${DOCKER_IMAGE}" \
DOCKER_IMAGE_TAG="${DOCKER_IMAGE_TAG}" \
"${CENTOS_DIR}/build-docker-rpmbuild.sh"

USER_ID=$(id -u)
GROUP_ID=$(id -g)

# NOTE: A shared volume has the same gid as its host volume on Linux,
# but the same group name on macOS. To run this script on both
# platforms, we run the build with the group name of the docker
# socket.
DOCKER_SOCKET_GID=$(echo /var/run/docker.sock | perl -lne 'use File::stat; print stat($_)->gid')

# Attach to terminal if we have a TTY so that things like CTRL-C work.
if [ -t 1 ]; then
  TTYARGS="-ti"
else
  TTYARGS=""
fi

docker run \
  $TTYARGS \
  --rm \
  --user "${USER_ID}:${GROUP_ID}" \
  --group-add 0 \
  --group-add "${DOCKER_SOCKET_GID}" \
  -v "${SOURCE_DIR}:${SOURCE_DIR}" \
  "${DOCKER_IMAGE}:${DOCKER_IMAGE_TAG}" \
  /bin/bash -c "MAKE_DIST=true MESOS_TAG=${MESOS_TAG} ${CENTOS_DIR}/build_rpm.sh"

echo "RPM has been built and can be found at:"
echo "${SOURCE_DIR}/centos${CENTOS_DISTRO}/rpmbuild/RPMS/x86_64"
