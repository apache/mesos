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

FROM arm64v8/ubuntu:16.04

# Install dependencies.
RUN apt-get update && \
    apt-get install -qy \
      autoconf \
      build-essential \
      clang \
      curl \
      git \
      iputils-ping \
      libacl1-dev \
      libapr1-dev \
      libcurl4-nss-dev \
      libev-dev \
      libevent-dev \
      libsasl2-dev \
      libssl-dev \
      libsvn-dev \
      libtool \
      maven \
      openjdk-8-jdk \
      python-dev \
      python-six \
      sed \
      zlib1g-dev \
      software-properties-common \
      python-software-properties && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists

# Install Python 3.6.
RUN add-apt-repository -y ppa:deadsnakes/ppa && \
    apt-get update && \
    apt-get install -qy \
      python3.6 \
      python3.6-dev \
      python3.6-venv && \
    add-apt-repository --remove -y ppa:deadsnakes/ppa && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists

# Install newer CMake.
RUN curl https://cmake.org/files/v3.8/cmake-3.8.2.tar.gz -o /tmp/cmake-3.8.2.tar.gz && \
    tar xvzf /tmp/cmake-3.8.2.tar.gz -C /tmp && \
    cd /tmp/cmake-3.8.2 && \
    ./configure && \
    make && \
    make install && \
    cmake --version

# Add an unprivileged user.
RUN adduser --disabled-password --gecos '' mesos
USER mesos

COPY ["entrypoint.sh", "entrypoint.sh"]
CMD ["./entrypoint.sh"]
