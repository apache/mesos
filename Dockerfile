#
# Dockerfile for building Mesos from source
#
# Create snapshot builds with:
# docker build -t mesos/mesos:git-`git rev-parse --short HEAD` .
#
# Run master/slave with:
# docker run mesos/mesos:git-`git rev-parse --short HEAD` mesos-master [options]
# docker run mesos/mesos:git-`git rev-parse --short HEAD` mesos-slave [options]
#
FROM ubuntu:14.04
MAINTAINER Gabriel Monroy <gabriel@opdemand.com>

# build packages
ENV DEBIAN_FRONTEND noninteractive
RUN apt-get update
RUN apt-get install -yq build-essential autoconf libtool zlib1g-dev
RUN apt-get install -yq libcurl4-nss-dev libsasl2-dev
RUN apt-get install -yq openjdk-6-jdk maven
RUN apt-get install -yq python-dev python-boto
RUN apt-get install -yq libsvn-dev libapr1-dev

# export environment
ENV JAVA_HOME /usr/lib/jvm/java-6-openjdk-amd64

# include libmesos on library path
ENV LD_LIBRARY_PATH /usr/local/lib

# copy local checkout into /opt
ADD . /opt

WORKDIR /opt

# configure
RUN ./bootstrap
RUN mkdir build && cd build && ../configure

WORKDIR /opt/build

# build and cleanup in a single layer
RUN make -j4 install && cd / && rm -rf /opt
