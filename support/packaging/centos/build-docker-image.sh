#!/usr/bin/env bash

# This script builds a CentOS based docker image with Mesos installed
# using the current head of the source tree.

CENTOS_DIR="$(cd "$(dirname "$0")"; pwd -P)"
SOURCE_DIR="$(cd $CENTOS_DIR/../../..; pwd -P)"

BUILD_IMAGE="mesos/packaging-centos7"
MESOS_IMAGE="mesos"

if ! [ -x "$(command -v docker)" ]; then
  echo 'Error: docker is not installed.' >&2
  exit 1
fi

if [ -d "$SOURCE_DIR/centos7" ]; then
  echo "Please cleanup 'centos7' under your Mesos source directory"
  exit 1
fi

# Build the image for building Mesos packages.
docker build \
  --rm \
  -t $BUILD_IMAGE \
  -f $SOURCE_DIR/support/packaging/centos/centos7.dockerfile \
  $SOURCE_DIR/support/packaging/centos/

# Build the RPM.
docker run \
  --rm \
  -v $SOURCE_DIR:$SOURCE_DIR \
  $BUILD_IMAGE \
  $SOURCE_DIR/support/packaging/centos/build_rpm.sh

# Build the image for running Mesos.
TMP_BUILD_DIR=$(mktemp -d)

cp $SOURCE_DIR/centos7/rpmbuild/RPMS/x86_64/*.rpm $TMP_BUILD_DIR

cat <<EOF > $TMP_BUILD_DIR/Dockerfile
FROM centos:7
ADD mesos-?.?.?-*.rpm /
RUN yum --nogpgcheck -y localinstall /mesos-*.rpm
EOF

docker build --rm -t $MESOS_IMAGE $TMP_BUILD_DIR

rm -rf $TMP_BUILD_DIR
