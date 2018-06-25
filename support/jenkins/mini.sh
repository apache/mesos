#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail -o verbose

CURRENT_DIR="$(cd "$(dirname "$0")"; pwd -P)"
SUPPORT_DIR="${CURRENT_DIR}/.."

: ${USERNAME:?"Environment variable 'USERNAME' must be set to the username of the 'Mesos DockerBot' Docker hub account."}
: ${PASSWORD:?"Environment variable 'PASSWORD' must be set to the password of the 'Mesos DockerBot' Docker hub account."}

MESOS_SHA=${MESOS_SHA:-`git rev-parse HEAD`}
MESOS_VERSION=`git describe --exact-match $MESOS_SHA 2>/dev/null || echo $MESOS_SHA`

echo "MESOS_SHA=$MESOS_SHA"
echo "MESOS_VERSION=$MESOS_VERSION"

DOCKER_IMAGE_MINI=${DOCKER_IMAGE_MINI:-"mesos/mesos-mini"}
DOCKER_IMAGE_PACKAGING=${DOCKER_IMAGE_PACKAGING:-"mesos/mesos-centos-packaging"}
DOCKER_IMAGE_DISTRO=${DOCKER_IMAGE_DISTRO:-"mesos/mesos-centos"}

if [[ "$MESOS_VERSION" != "$MESOS_SHA" ]]; then
  # HEAD is also a tag.
  DOCKER_IMAGE_TAG="$MESOS_VERSION"
else
  DOCKER_IMAGE_TAG=`date +%F`
fi

function cleanup {
  docker rmi $(docker images -q ${DOCKER_IMAGE_PACKAGING}:${DOCKER_IMAGE_TAG}) || true
  docker rmi $(docker images -q ${DOCKER_IMAGE_DISTRO}:${DOCKER_IMAGE_TAG}) || true
  docker rmi $(docker images -q ${DOCKER_IMAGE_MINI}:${DOCKER_IMAGE_TAG}) || true
}

trap cleanup EXIT

DOCKER_IMAGE_MINI=${DOCKER_IMAGE_MINI} \
DOCKER_IMAGE_PACKAGING=${DOCKER_IMAGE_PACKAGING} \
DOCKER_IMAGE_DISTRO=${DOCKER_IMAGE_DISTRO} \
DOCKER_IMAGE_TAG=${DOCKER_IMAGE_TAG} \
"${SUPPORT_DIR}/mesos-mini/build.sh"

docker tag ${DOCKER_IMAGE_MINI}:${DOCKER_IMAGE_TAG} ${DOCKER_IMAGE_MINI}:latest
docker login -u ${USERNAME} -p ${PASSWORD}
docker push ${DOCKER_IMAGE_MINI}:${DOCKER_IMAGE_TAG}
docker push ${DOCKER_IMAGE_MINI}:latest
