#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail -o verbose

# This script builds a Docker image that will be used to build RPM
# package for Mesos.

CENTOS_DIR="$(cd "$(dirname "$0")"; pwd -P)"

CENTOS_DISTRO=${CENTOS_DISTRO:-"7"}
DOCKER_IMAGE=${DOCKER_IMAGE:-"mesos/mesos-centos${CENTOS_DISTRO}-rpmbuild"}
DOCKER_IMAGE_TAG=${DOCKER_IMAGE_TAG:-"latest"}

if ! [ -x "$(command -v docker)" ]; then
  echo 'Error: docker is not installed.' >&2
  exit 1
fi

USER_NAME=$(id -u -n)
USER_ID=$(id -u)
GROUP_NAME=$(id -g -n)
GROUP_ID=$(id -g)

# Build the image for building Mesos packages.
docker build \
  --rm \
  --build-arg "USER_NAME=${USER_NAME}" \
  --build-arg "USER_ID=${USER_ID}" \
  --build-arg "GROUP_NAME=${GROUP_NAME}" \
  --build-arg "GROUP_ID=${GROUP_ID}" \
  -t "${DOCKER_IMAGE}:${DOCKER_IMAGE_TAG}" \
  -f "${CENTOS_DIR}/centos${CENTOS_DISTRO}.dockerfile" \
  "${CENTOS_DIR}"
