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
CURRENT_BRANCHES=$(git branch --points-at HEAD -r)
RELEASE_BRANCH=$(echo "${CURRENT_BRANCHES}" | grep -E 'origin/[0-9]*\.[0-9]*\.x' | grep -E -o '[0-9]*\.[0-9]*\.x' || true)

if [ -z "${RELEASE_BRANCH}" ]; then
  RELEASE_BRANCH="master"
fi

DATE=$(date +%F)

if [[ "${MESOS_TAG_OR_SHA}" != "${MESOS_SHA}" ]]; then
  # HEAD is also a tag.
  DOCKER_IMAGE_TAG="${MESOS_TAG_OR_SHA}"
  DOCKER_IMAGE_LATEST_TAG=""
else
  DOCKER_IMAGE_TAG="${RELEASE_BRANCH}-${DATE}"
  DOCKER_IMAGE_LATEST_TAG="${RELEASE_BRANCH}"
fi

echo "MESOS_SHA=${MESOS_SHA}"
echo "RELEASE_BRANCH=${RELEASE_BRANCH}"
echo "DOCKER_IMAGE_TAG=${DOCKER_IMAGE_TAG}"

function cleanup {
  docker rmi "${DOCKER_IMAGE}:${DOCKER_IMAGE_LATEST_TAG}" || true
  docker rmi "${DOCKER_IMAGE}:${DOCKER_IMAGE_TAG}" || true
}

trap cleanup EXIT

CENTOS_DISTRO="7" \
DOCKER_IMAGE=${DOCKER_IMAGE} \
DOCKER_IMAGE_TAG=${DOCKER_IMAGE_TAG} \
"${SUPPORT_DIR}/packaging/centos/build-docker-centos.sh"

docker login -u "${USERNAME}" -p "${PASSWORD}"
docker push "${DOCKER_IMAGE}:${DOCKER_IMAGE_TAG}"

if [ ! -z "${DOCKER_IMAGE_LATEST_TAG}" ]; then
  docker tag "${DOCKER_IMAGE}:${DOCKER_IMAGE_TAG}" "${DOCKER_IMAGE}:${DOCKER_IMAGE_LATEST_TAG}"
  docker push "${DOCKER_IMAGE}:${DOCKER_IMAGE_LATEST_TAG}"
fi
