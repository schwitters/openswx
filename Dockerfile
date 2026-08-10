# syntax=docker/dockerfile:1

FROM debian:trixie-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    git \
    libasio-dev \
    libssl-dev \
    libpugixml-dev \
    libsqlite3-dev \
    libzip-dev \
    libz-dev \
    nlohmann-json3-dev \
    pkg-config \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
RUN cmake --build build -j"$(nproc)" --target asmbox swx_dump swx_scan

FROM debian:trixie-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3t64 \
    libpugixml1v5 \
    libsqlite3-0 \
    libstdc++6 \
    libzip5 \
    zlib1g \
  && rm -rf /var/lib/apt/lists/*

RUN groupadd --system --gid 10001 openswx \
  && useradd --system --uid 10001 --gid 10001 \
      --create-home --home-dir /var/lib/openswx openswx

WORKDIR /opt/openswx

COPY --from=builder /src/build/asmbox/asmbox /usr/local/bin/asmbox
COPY --from=builder /src/build/asmbox/swx_dump /usr/local/bin/swx_dump
COPY --from=builder /src/build/asmbox/swx_scan /usr/local/bin/swx_scan
COPY --from=builder /src/asmbox/templates /opt/openswx/templates

RUN mkdir -p /data \
  && chown -R openswx:openswx /data /opt/openswx /var/lib/openswx

ENV ASMBOX_BIND_ADDR=0.0.0.0
ENV ASMBOX_PORT=8087
ENV ASMBOX_DATA_DIR=/data
ENV ASMBOX_TEMPLATE_DIR=/opt/openswx/templates

EXPOSE 8087
VOLUME ["/data"]

USER openswx

ENTRYPOINT ["asmbox"]
