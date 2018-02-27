#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail -o verbose

# This script build and push Docker image that has Mesos distro
# installed (based on CentOS).

CURRENT_DIR="$(cd "$(dirname "$0")"; pwd -P)"
SUPPORT_DIR="${CURRENT_DIR}/.."

: ${USERNAME:?"Environment variable 'USERNAME' must be set to the username of the 'Mesos DockerBot' Docker hub account."}
: ${PASSWORD:?"Environment variable 'PASSWORD' must be set to the password of the 'Mesos DockerBot' Docker hub account."}

DOCKER_IMAGE_PACKAGING=${DOCKER_IMAGE_PACKAGING:-"mesos/mesos-centos-packaging"}
DOCKER_IMAGE_DISTRO=${DOCKER_IMAGE_DISTRO:-"mesos/mesos-centos"}
DOCKER_IMAGE_TAG=`date +%F`

function cleanup {
  docker rmi $(docker images -q ${DOCKER_IMAGE_PACKAGING}:${DOCKER_IMAGE_TAG}) || true
  docker rmi $(docker images -q ${DOCKER_IMAGE_DISTRO}:${DOCKER_IMAGE_TAG}) || true
}

trap cleanup EXIT

DOCKER_IMAGE_PACKAGING=${DOCKER_IMAGE_PACKAGING} \
DOCKER_IMAGE_DISTRO=${DOCKER_IMAGE_DISTRO} \
DOCKER_IMAGE_TAG=${DOCKER_IMAGE_TAG} \
"${SUPPORT_DIR}/packaging/centos/build-docker-image.sh"

docker tag ${DOCKER_IMAGE_DISTRO}:${DOCKER_IMAGE_TAG} ${DOCKER_IMAGE_DISTRO}:latest
docker login -u ${USERNAME} -p ${PASSWORD}
docker push ${DOCKER_IMAGE_DISTRO}:${DOCKER_IMAGE_TAG}
docker push ${DOCKER_IMAGE_DISTRO}:latest
