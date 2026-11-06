FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ gcc make openjdk-21-jre-headless ca-certificates zlib1g-dev file curl \
    && rm -rf /var/lib/apt/lists/*
# aeron 1.53.1 needs cmake >= 3.30; ubuntu 24.04 ships 3.28
RUN ARCH=$(uname -m) && \
    curl -sfL "https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-${ARCH}.tar.gz" \
        -o /tmp/cmake.tgz && \
    tar -xzf /tmp/cmake.tgz -C /opt && rm /tmp/cmake.tgz && \
    ln -s /opt/cmake-3.31.6-linux-${ARCH}/bin/cmake /usr/local/bin/cmake
WORKDIR /spike
