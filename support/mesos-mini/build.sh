#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail -o verbose

# This script builds a Docker image that has one master, one agent and
# one example framework (Marathon) running.

CURRENT_DIR="$(cd "$(dirname "$0")"; pwd -P)"
SUPPORT_DIR="${CURRENT_DIR}/.."

DOCKER_IMAGE=${DOCKER_IMAGE_MINI:-"mesos/mesos-mini"}
DOCKER_IMAGE_TAG=${DOCKER_IMAGE_TAG:-"latest"}
DOCKER_IMAGE_DISTRO=${DOCKER_IMAGE_DISTRO:-"mesos/mesos-centos"}

if ! [ -x "$(command -v docker)" ]; then
  echo 'Error: docker is not installed.' >&2
  exit 1
fi

DOCKER_IMAGE=${DOCKER_IMAGE_DISTRO} \
DOCKER_IMAGE_TAG=${DOCKER_IMAGE_TAG} \
"${SUPPORT_DIR}/packaging/centos/build-docker-centos.sh"

docker build \
  --build-arg "BASE_IMAGE_NAME=${DOCKER_IMAGE_DISTRO}:${DOCKER_IMAGE_TAG}" \
  -t "${DOCKER_IMAGE}:${DOCKER_IMAGE_TAG}" \
  "${CURRENT_DIR}/"
