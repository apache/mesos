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

FROM ubuntu:trusty
MAINTAINER The Apache Mesos Developers <dev@mesos.apache.org>

WORKDIR /tmp/build

# Build Mesos-flavored `clang-tidy`.
RUN apt-get update
RUN apt-get install -qy --no-install-recommends \
  build-essential \
  ca-certificates \
  cmake \
  git \
  python-dev && \
  apt-get clean

RUN \
  git clone --depth 1 -b release_38 http://llvm.org/git/llvm /tmp/llvm && \
  git clone --depth 1 -b release_38 http://llvm.org/git/clang /tmp/llvm/tools/clang && \
  git clone --depth 1 -b mesos_38 http://github.com/mesos/clang-tools-extra.git /tmp/llvm/tools/clang/tools/extra && \
  \
  cmake /tmp/llvm -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/ && \
  make -j $(nproc) install && \
  \
  cd / && \
  rm -rf /tmp/llvm && \
  rm -rf /tmp/build

# Install Bear.
RUN apt-get install -qy \
  curl && \
  apt-get clean

RUN \
  curl -k -O https://codeload.github.com/rizsotto/Bear/tar.gz/2.2.0 && \
  tar xzf 2.2.0 && \
  rm 2.2.0 && \
  cd Bear-2.2.0/ && \
  cmake . -DCMAKE_INSTALL_PREFIX=/opt && \
  make -j $(nproc) install && \
  \
  cd / && \
  rm -rf Bear-2.2.0

ENV PATH /opt/bin:$PATH

# Install Mesos dependencies
RUN apt-get install -qy \
  autoconf \
  jq \
  libapr1-dev \
  libcurl4-nss-dev \
  libev-dev \
  libevent-dev \
  libsasl2-dev \
  libsasl2-modules \
  libsvn-dev \
  libtool \
  maven \
  openjdk-7-jdk \
  zlib1g-dev && \
  apt-get clean

# Install parallel for clang-tidy invocation.
RUN apt-get install -qy \
  parallel

# Wire up the script which performs the actual work.
WORKDIR /BUILD
ADD ["entrypoint.sh", "entrypoint.sh"]
CMD exec ./entrypoint.sh
