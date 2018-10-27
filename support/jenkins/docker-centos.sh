#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail -o verbose

# This script build and push Docker image that has Mesos distro
# installed (based on CentOS).

CURRENT_DIR="$(cd "$(dirname "$0")"; pwd -P)"
SUPPORT_DIR="${CURRENT_DIR}/.."

: "${USERNAME:?"Environment variable 'USERNAME' must be set to the username of the 'Mesos DockerBot' Docker hub account."}"
: "${PASSWORD:?"Environment variable 'PASSWORD' must be set to the password of the 'Mesos DockerBot' Docker hub account."}"

DOCKER_IMAGE=${DOCKER_IMAGE:-"mesos/mesos-centos"}

MESOS_SHA=${MESOS_SHA:-$(git rev-parse HEAD)}
MESOS_TAG_OR_SHA=$(git describe --exact-match "${MESOS_SHA}" 2>/dev/null || echo "${MESOS_SHA}")
DOCKER_IMAGE_TAG="${MESOS_TAG_OR_SHA}"

echo "MESOS_SHA=${MESOS_SHA}"
echo "DOCKER_IMAGE_TAG=${DOCKER_IMAGE_TAG}"

function cleanup {
  docker rmi "$(docker images -q "${DOCKER_IMAGE}:${DOCKER_IMAGE_TAG}")" || true
}

trap cleanup EXIT

CENTOS_DISTRO="7" \
DOCKER_IMAGE=${DOCKER_IMAGE} \
DOCKER_IMAGE_TAG=${DOCKER_IMAGE_TAG} \
"${SUPPORT_DIR}/packaging/centos/build-docker-centos.sh"

DATE=$(date +%F)

docker tag "${DOCKER_IMAGE}:${DOCKER_IMAGE_TAG}" "${DOCKER_IMAGE}:${DATE}"
docker login -u "${USERNAME}" -p "${PASSWORD}"
docker push "${DOCKER_IMAGE}:${DOCKER_IMAGE_TAG}"
docker push "${DOCKER_IMAGE}:${DATE}"
