# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at

#     http://www.apache.org/licenses/LICENSE-2.0

# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

FROM centos:7

# Add repo for newer Clang.
# We don't use `llvm-toolset-7` due to a `devtoolset-7` bug that we run into
# with CMake builds: https://bugzilla.redhat.com/show_bug.cgi?id=1519073
RUN curl -sSL https://copr.fedorainfracloud.org/coprs/alonid/llvm-3.9.0/repo/epel-7/alonid-llvm-3.9.0-epel-7.repo \
         -o /etc/yum.repos.d/llvm-3.9.0.repo

# Install dependencies.
RUN yum groupinstall -y 'Development Tools' && \
    yum install -y centos-release-scl && \
    yum install -y \
      apr-devel \
      apr-utils-devel \
      clang-3.9.0 \
      cyrus-sasl-devel \
      cyrus-sasl-md5 \
      devtoolset-4-gcc-c++ \
      git \
      java-1.8.0-openjdk-devel \
      libcurl-devel \
      libevent-devel \
      libev-devel \
      maven \
      openssl-devel \
      python-devel \
      python-six \
      subversion-devel \
      which \
      zlib-devel && \
    yum clean all && \
    rm -rf /var/cache/yum

# Install Python 3.6 and pip.
# We need two separate `yum install` in order for
# the python packages to be installed correctly.
RUN yum install -y \
      https://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm && \
    yum install -y \
      python36 \
      python36-devel && \
    yum clean all && \
    rm -rf /var/cache/yum

# Use update-alternatives to set python3.6 as python3.
RUN update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.6 1

# Install pip for Python 3.6.
RUN curl https://bootstrap.pypa.io/get-pip.py | python3

# Install virtualenv to /usr/bin/virtualenv with pip.
RUN pip3 install --no-cache-dir virtualenv

# Install newer CMake.
RUN curl -sSL https://cmake.org/files/v3.8/cmake-3.8.2-Linux-x86_64.sh \
         -o /tmp/install-cmake.sh && \
    sh /tmp/install-cmake.sh --skip-license --prefix=/usr/local && \
    rm -f /tmp/install-cmake.sh

ENV PATH /opt/llvm-3.9.0/bin:$PATH

# Add an unprivileged user.
RUN adduser mesos
USER mesos

COPY ["enable-devtoolset-4.sh", "/etc/profile.d"]
COPY ["entrypoint.sh", "entrypoint.sh"]
CMD ["bash", "-cl", "./entrypoint.sh"]
