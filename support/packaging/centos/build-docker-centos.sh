#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail -o verbose

# This script builds a CentOS based docker image with Mesos installed
# using the current head of the source tree.

CENTOS_DIR="$(cd "$(dirname "$0")"; pwd -P)"
SOURCE_DIR="$(cd "${CENTOS_DIR}/../../.."; pwd -P)"

CENTOS_DISTRO=${CENTOS_DISTRO:-"7"}
DOCKER_IMAGE=${DOCKER_IMAGE:-"mesos/mesos-centos"}
DOCKER_IMAGE_TAG=${DOCKER_IMAGE_TAG:-"latest"}

if ! [ -x "$(command -v docker)" ]; then
  echo 'Error: docker is not installed.' >&2
  exit 1
fi

"${CENTOS_DIR}/build-rpm-docker.sh"

# Build the image for running Mesos.
DOCKER_CONTEXT_DIR="${SOURCE_DIR}/centos${CENTOS_DISTRO}/rpmbuild/RPMS/x86_64"

cat <<EOF > "${DOCKER_CONTEXT_DIR}/Dockerfile"
FROM centos:${CENTOS_DISTRO}
ADD mesos-?.?.?-*.rpm /
RUN yum --nogpgcheck -y localinstall /mesos-*.rpm
EOF

docker build \
  --rm \
  -t "${DOCKER_IMAGE}:${DOCKER_IMAGE_TAG}" \
  "${DOCKER_CONTEXT_DIR}"
