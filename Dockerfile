FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ARG APT_MIRROR=http://mirrors.tencentyun.com/ubuntu

RUN sed -i "s|http://archive.ubuntu.com/ubuntu|${APT_MIRROR}|g; s|http://security.ubuntu.com/ubuntu|${APT_MIRROR}|g" /etc/apt/sources.list \
    && apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    default-libmysqlclient-dev \
    default-mysql-client \
    libhiredis-dev \
    curl \
    python3 \
    libssl-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY . /app

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --target server --parallel

EXPOSE 9006

CMD ["./build/server"]
