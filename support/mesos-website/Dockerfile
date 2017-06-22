FROM centos:7
MAINTAINER "dev@mesos.apache.org"

LABEL Description="This image is used for generating Mesos web site."

# Install Mesos build dependencies.
RUN yum install -y which
RUN yum groupinstall -y 'Development Tools'
RUN yum install -y epel-release
RUN yum install -y clang git maven cmake
RUN yum install -y java-1.8.0-openjdk-devel python-devel zlib-devel libcurl-devel openssl-devel cyrus-sasl-devel cyrus-sasl-md5 apr-devel subversion-devel apr-utils-devel libevent-devel libev-devel

# Install website dependencies.
RUN yum install -y ruby ruby-devel doxygen
RUN gem install bundler


# Setup environment to build Mesos and website.
ENV CC gcc
ENV CXX g++
ENV LANG en_US.UTF-8
ENV LC_ALL en_US.UTF-8


WORKDIR /mesos
CMD bash support/mesos-website/entrypoint.sh
