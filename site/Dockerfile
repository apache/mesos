FROM mesos/mesos-build:ubuntu-16.04
MAINTAINER "dev@mesos.apache.org"

LABEL Description="This image is used for generating Mesos web site from local sources and serving it on port 4567 (livereload on port 35729)."

# The mesos build image drops down to user `mesos`, but
# we need priviledged access to install packages below.
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

EXPOSE 4567
EXPOSE 35729

WORKDIR /mesos/site
CMD bash entrypoint.sh
