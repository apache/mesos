FROM ubuntu:14.04
MAINTAINER "dev@mesos.apache.org"

LABEL Description="This image is used for generating Mesos web site from local sources and serving it on port 4567."

RUN apt-get update
RUN apt-get install -y subversion g++ ruby ruby-dev make rake nodejs git
RUN gem install bundler

ENV LANG C.UTF-8
ENV LANGUAGE C.UTF08
ENV LC_ALL C.UTF-8

EXPOSE 4567

WORKDIR /mesos/site
CMD bundle install && rake && rake dev
