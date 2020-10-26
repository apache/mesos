FROM centos:6
MAINTAINER Kapil Arya <kapil@apache.org>

RUN yum install -y              \
      centos-release-scl        \
      epel-release              \
      git                       \
      redhat-rpm-config         \
      rpm-build                 \
      scl-utils                 \
      yum-utils

# We need to install `devtoolset-7` in a separate step because the
# repository containing it only gets added during installation of
# the `centos-release-scl` package.
RUN yum install -y devtoolset-7

# Add the Subversion repo.
RUN echo -e '[WANdiscoSVN]\n\
name=WANdisco SVN Repo 1.11\n\
enabled=1\n\
baseurl=http://opensource.wandisco.com/centos/6/svn-1.11/RPMS/\$basearch/\n\
gpgcheck=1\n\
gpgkey=http://opensource.wandisco.com/RPM-GPG-KEY-WANdisco' \
>> /etc/yum.repos.d/wandisco-svn.repo

# Add the Apache Maven repo.
RUN curl -sSL \
      http://repos.fedorapeople.org/repos/dchen/apache-maven/epel-apache-maven.repo \
      -o /etc/yum.repos.d/epel-apache-maven.repo

# PostgreSQL repo for libevent2.
RUN  rpm -Uvh --replacepkgs \
      http://yum.postgresql.org/9.5/redhat/rhel-6-x86_64/pgdg-redhat-repo-latest.noarch.rpm

# Setup JDK
RUN echo -e 'export JAVA_HOME=/usr/lib/jvm/java-openjdk' >> /etc/profile.d/java-home.sh

ADD mesos.spec /mesos.spec

RUN yum makecache && \
    yum-builddep -y /mesos.spec

ADD user-init.sh /user-init.sh

ARG USER_NAME=root
ARG USER_ID=0
ARG GROUP_NAME=root
ARG GROUP_ID=0

RUN /user-init.sh $USER_NAME $USER_ID $GROUP_NAME $GROUP_ID
