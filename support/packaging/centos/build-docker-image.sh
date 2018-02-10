#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail -o verbose

# This script builds a CentOS based docker image with Mesos installed
# using the current head of the source tree.

CENTOS_DIR="$(cd "$(dirname "$0")"; pwd -P)"
SOURCE_DIR="$(cd ${CENTOS_DIR}/../../..; pwd -P)"

DOCKER_IMAGE_PACKAGING=${DOCKER_IMAGE_PACKAGING:-"mesos/mesos-centos-packaging"}
DOCKER_IMAGE_DISTRO=${DOCKER_IMAGE_DISTRO:-"mesos/mesos-centos"}
DOCKER_IMAGE_TAG=${DOCKER_IMAGE_TAG:-"latest"}

if ! [ -x "$(command -v docker)" ]; then
  echo 'Error: docker is not installed.' >&2
  exit 1
fi

if [ -d "${SOURCE_DIR}/centos7" ]; then
  echo "Please cleanup 'centos7' under your Mesos source directory"
  exit 1
fi

TMP_BUILD_DIR=$(mktemp -d)

function cleanup {
  rm -rf "${TMP_BUILD_DIR}"
  rm -rf "${SOURCE_DIR}/centos7"
}

trap cleanup EXIT

# Build the image for building Mesos packages.
docker build \
  --rm \
  -t ${DOCKER_IMAGE_PACKAGING}:${DOCKER_IMAGE_TAG} \
  -f "${SOURCE_DIR}/support/packaging/centos/centos7.dockerfile" \
  "${SOURCE_DIR}/support/packaging/centos/"

# Build the RPM.
USER_ID=`id -u`
GROUP_ID=`id -g`

docker run \
  --rm \
  -v "${SOURCE_DIR}:${SOURCE_DIR}" \
  ${DOCKER_IMAGE_PACKAGING}:${DOCKER_IMAGE_TAG} \
  /bin/bash -c "${SOURCE_DIR}/support/packaging/centos/build_rpm.sh && chown -R ${USER_ID}:${GROUP_ID} ${SOURCE_DIR}/centos7"

# Build the image for running Mesos.
cp "${SOURCE_DIR}"/centos7/rpmbuild/RPMS/x86_64/*.rpm "${TMP_BUILD_DIR}"

cat <<EOF > "${TMP_BUILD_DIR}/Dockerfile"
FROM centos:7
ADD mesos-?.?.?-*.rpm /
RUN yum --nogpgcheck -y localinstall /mesos-*.rpm
EOF

docker build --rm -t ${DOCKER_IMAGE_DISTRO}:${DOCKER_IMAGE_TAG} ${TMP_BUILD_DIR}
