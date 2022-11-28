FROM s390x/ubuntu:18.04

# Install dependencies.
RUN apt-get update && \
    apt-get install -qy \
      autoconf \
      bzip2 \
      build-essential \
      clang \
      curl \
      git \
      iputils-ping \
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
      vim cmake \
      software-properties-common \
      zlib1g-dev && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists

#RUN apt remove -y openjdk-11-jre-headless

# Install Python 3.6.
RUN add-apt-repository -y ppa:deadsnakes/ppa && \
    apt-get update && \
    apt-get install -qy \
      python3.6 \
      python3.6-dev \
      python3.6-distutils \
      python3.6-venv && \
    add-apt-repository --remove -y ppa:deadsnakes/ppa && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists

# Use update-alternatives to set python3.6 as python3.
RUN update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.6 1

# Install pip for Python 3.6.
RUN curl https://bootstrap.pypa.io/pip/3.6/get-pip.py | python3

ENV JAVA_HOME=/usr/lib/jvm/java-1.8.0-openjdk-s390x
ENV JAVA_TOOL_OPTIONS='-Xmx2048M'
ENV PATH=$JAVA_HOME/bin:$PATH

# Add an unprivileged user.
RUN adduser --disabled-password --gecos '' mesos
USER mesos

COPY ["entrypoint.sh", "entrypoint.sh"]
CMD ["./entrypoint.sh"]
