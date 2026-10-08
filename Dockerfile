FROM ubuntu:22.04

ARG ENABLE_PRINT
ENV ENABLE_PRINT=${ENABLE_PRINT}

WORKDIR /app

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ca-certificates \
    pkg-config \
    git \
    libcurl4-openssl-dev \
    libssl-dev \
    python3 \
    python3-pip\
    iproute2 \
    && rm -rf /var/lib/apt/lists/*

COPY ./requirements.txt ./requirements.txt
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

RUN ./recompile.sh

EXPOSE 7001 8001 4443 4444 4445 3601 8554
