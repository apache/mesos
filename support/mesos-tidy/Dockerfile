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

FROM ubuntu:xenial
MAINTAINER The Apache Mesos Developers <dev@mesos.apache.org>

WORKDIR /tmp/build

# Build Mesos-flavored `clang-tidy`.
RUN apt-get update && \
  apt-get install -qy --no-install-recommends \
  build-essential \
  ca-certificates \
  curl \
  git \
  python-dev && \
  apt-get clean && \
  rm -rf /var/lib/apt/lists/*

# Mesos requires at least cmake-3.7.0 on Linux and cmake-3.8.0 on Windows.
#
# TODO(abudnik): Skip this step when a newer version of CMake package is
# available in OS repository.
RUN curl -sSL https://cmake.org/files/v3.16/cmake-3.16.0-Linux-x86_64.sh \
    -o /tmp/install-cmake.sh && \
    sh /tmp/install-cmake.sh --skip-license --prefix=/usr/local

RUN \
  git clone --depth 1 -b release_90 http://llvm.org/git/llvm /tmp/llvm && \
  git clone --depth 1 -b mesos_90 http://github.com/mesos/clang.git /tmp/llvm/tools/clang && \
  git clone --depth 1 -b mesos_90 http://github.com/mesos/clang-tools-extra.git /tmp/llvm/tools/clang/tools/extra && \
  \
  cmake /tmp/llvm -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt && \
  cmake --build tools/clang/lib/Headers --target install -- -j $(nproc) && \
  cmake --build tools/clang/tools/extra/clang-tidy --target install -- -j $(nproc) && \
  \
  cd / && \
  rm -rf /tmp/llvm && \
  rm -rf /tmp/build

ENV PATH /opt/bin:$PATH

# Install Mesos dependencies
# TODO(mpark): Remove `libssl-dev` from this list once `MESOS-6942` is resolved.
RUN apt-get update && \
  apt-get install -qy \
  autoconf \
  libapr1-dev \
  libcurl4-nss-dev \
  libsasl2-dev \
  libsasl2-modules \
  libssl-dev \
  libsvn-dev \
  libtool \
  zlib1g-dev && \
  apt-get clean && \
  rm -rf /var/lib/apt/lists/*

# Install `jq` and `parallel` for `clang-tidy` invocation.
RUN apt-get update && \
  apt-get install -qy \
  jq \
  parallel && \
  apt-get clean && \
  rm -rf /var/lib/apt/lists/*

# Wire up the script which performs the actual work.
WORKDIR /BUILD
ADD ["entrypoint.sh", "entrypoint.sh"]
CMD exec ./entrypoint.sh
