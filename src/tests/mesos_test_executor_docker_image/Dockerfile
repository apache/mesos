FROM stackbrew/ubuntu:13.10
MAINTAINER Timothy Chen <tnachen@apache.org>

ADD http://downloads.mesosphere.io/master/ubuntu/13.10/mesos-test-executor.deb /tmp/mesos.deb

RUN ["env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "update"]
RUN ["env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "install", "-y", "--fix-missing", "--force-yes", "libsasl2-2", "libcurl3"]
RUN ["env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "install", "-y", "--fix-missing", "--force-yes", "default-jre-headless"]

RUN ["env", "DEBIAN_FRONTEND=noninteractive", "dpkg", "-i", "/tmp/mesos.deb"]
RUN ["bash", "-c", "echo manual > /etc/init/mesos-master.override"]
RUN ["bash", "-c", "echo manual > /etc/init/mesos-slave.override"]

RUN ["rm", "-rf", "/tmp/mesos.deb"]

CMD ["true"]
