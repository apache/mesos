FROM mesos/mesos-build:ubuntu-16.04
MAINTAINER "dev@mesos.apache.org"

LABEL Description="This image is used for generating Mesos web site."

# Set the root user explicitly because the base image
# `mesos/mesos-build` uses `mesos` user by default. Also, note that
# `root` inside the container is mapped to `jenkins` user on the build
# machine host in ASF CI.
USER root

# Install dependencies.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      doxygen \
      locales \
      ruby \
      ruby-dev \
      rubygems && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

RUN gem install bundler

RUN locale-gen en_US.UTF-8
ENV LANG en_US.UTF-8
ENV LC_ALL en_US.UTF-8

ENV CC gcc
ENV CXX g++

WORKDIR /mesos
CMD bash support/mesos-website/entrypoint.sh
