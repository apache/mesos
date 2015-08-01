#!/bin/bash

set -xe

# This is the script used by ASF Jenkins to build and check Mesos for
# a given OS and compiler combination.

# Require the following environment variables to be set.
: ${OS:?"Environment variable 'OS' must be set"}
: ${COMPILER:?"Environment variable 'COMPILER' must be set"}
: ${CONFIGURATION:?"Environment variable 'CONFIGURATION' must be set"}

# Change to the root of Mesos repo for docker build context.
MESOS_DIRECTORY=$( cd "$( dirname "$0" )/.." && pwd )
cd "$MESOS_DIRECTORY"

# TODO(vinod): Once ASF CI supports Docker 1.5 use a custom name for
# Dockerfile to avoid overwriting Dockerfile (if it exists) at the root
# of the repo.
DOCKERFILE="Dockerfile"

rm -f $DOCKERFILE # Just in case a stale one exists.

# Helper function that appends instructions to docker file.
function append_dockerfile {
   echo $1 >> $DOCKERFILE
}


# TODO(vinod): Add support for Fedora and Debian.
case $OS in
  centos*)
    # NOTE: Currently we only support CentOS7+ due to the
    # compiler versions needed to compile Mesos.

    append_dockerfile "FROM $OS"

    # Install dependencies.

    append_dockerfile "RUN yum groupinstall -y 'Development Tools'"
    append_dockerfile "RUN yum install -y epel-release" # Needed for clang.
    append_dockerfile "RUN yum install -y clang git maven"
    append_dockerfile "RUN yum install -y java-1.7.0-openjdk-devel python-devel zlib-devel libcurl-devel openssl-devel cyrus-sasl-devel cyrus-sasl-md5 apr-devel subversion-devel apr-utils-devel libevent-devel"

    # Add an unprivileged user.
    append_dockerfile "RUN adduser mesos"
   ;;
  ubuntu*)
    # NOTE: Currently we only support Ubuntu13.10+ due to the
    # compiler versions needed to compile Mesos.

    append_dockerfile "FROM $OS"

    # Install dependencies.
    append_dockerfile "RUN apt-get update"
    append_dockerfile "RUN apt-get -y install build-essential clang git maven autoconf libtool"
    append_dockerfile "RUN apt-get -y install openjdk-7-jdk python-dev python-boto libcurl4-nss-dev libsasl2-dev libapr1-dev libsvn-dev libevent-dev"

    # Add an unpriviliged user.
    append_dockerfile "RUN adduser --disabled-password --gecos '' mesos"

    # Disable any tests failing on Ubuntu.
    append_dockerfile "ENV GTEST_FILTER -FsTest.FileSystemTableRead"
    ;;
  *)
    echo "Unknown OS $OS"
    exit 1
    ;;
esac

case $COMPILER in
  gcc)
    append_dockerfile "ENV CC gcc"
    append_dockerfile "ENV CXX g++"
    ;;
  clang)
    append_dockerfile "ENV CC clang"
    append_dockerfile "ENV CXX clang++"
    ;;
  *)
    echo "Unknown Compiler $COMPILER"
    exit 1
    ;;
esac

# Set working directory.
append_dockerfile "WORKDIR mesos"

# Copy Mesos source tree into the image.
append_dockerfile "COPY . /mesos/"

# NOTE: We run all the tests as unprivileged 'mesos' user because
# we need to fix cgroups related tests to work inside Docker; certain
# tests (e.g., ContainerizerTest) enable cgroups isolation if the user
# is 'root'.
# TODO(vinod): Fix cgroups tests to work inside Docker.
append_dockerfile "RUN chown -R mesos /mesos"
append_dockerfile "USER mesos"

# Build and check Mesos.
append_dockerfile "CMD ./bootstrap && ./configure $CONFIGURATION && DISTCHECK_CONFIGURE_FLAGS=\"$CONFIGURATION\" GLOG_v=1 MESOS_VERBOSE=1 make -j8 distcheck"

# Generate a random image tag.
TAG=mesos-`date +%s`-$RANDOM

# Build the Docker imeage.
# TODO(vinod): Instead of building Docker images on the fly host the
# images on DockerHub and use them.
docker build -t $TAG .

# Set a trap to delete the image on exit.
trap "docker rmi $TAG" EXIT

# Uncomment below to print kernel log incase of failures.
# trap "dmesg" ERR

# Now run the image.
# NOTE: We run in 'privileged' mode to circumvent permission issues
# with AppArmor. See https://github.com/docker/docker/issues/7276.
docker run --privileged --rm $TAG
