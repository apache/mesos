FROM centos:7
MAINTAINER "dev@mesos.apache.org"

LABEL Description="This image is used for generating Mesos web site from local sources and serving it on port 4567."

RUN yum install -y gcc-c++ make ruby ruby-devel rake doxygen java-1.7.0-openjdk-devel
RUN gem install bundler

ENV LANG en_US.UTF-8
ENV LC_ALL en_US.UTF-8

EXPOSE 4567

WORKDIR /mesos/site
CMD bash build.sh
