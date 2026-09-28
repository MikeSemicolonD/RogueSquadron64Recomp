# Ephemeral self-hosted GitHub Actions runner that builds the Linux release.
# The ROM-derived inputs are bind-mounted read-only at /rs64-inputs at run time (see ci/linux/run-runner.ps1); nothing ROM-derived is baked into the image.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG RUNNER_VERSION=latest

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git python3 \
        libsdl2-dev libvulkan-dev libgtk-3-dev \
        gcc-mips-linux-gnu binutils-mips-linux-gnu \
        binutils file curl jq ca-certificates tar gzip zip unzip \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -m -s /bin/bash runner
WORKDIR /home/runner/actions-runner

RUN set -eux; \
    ver="${RUNNER_VERSION}"; \
    if [ "$ver" = "latest" ]; then \
        ver="$(curl -fsSL https://api.github.com/repos/actions/runner/releases/latest | jq -r .tag_name | sed 's/^v//')"; \
    fi; \
    curl -fsSL -o runner.tgz "https://github.com/actions/runner/releases/download/v${ver}/actions-runner-linux-x64-${ver}.tar.gz"; \
    tar xzf runner.tgz; rm runner.tgz; \
    ./bin/installdependencies.sh; \
    chown -R runner:runner /home/runner

COPY entrypoint.sh /usr/local/sbin/rs64-entrypoint.sh
RUN chmod 0700 /usr/local/sbin/rs64-entrypoint.sh

ENV RS64_INPUTS=/rs64-inputs \
    RS64_MIPS_GCC=/usr/bin/mips-linux-gnu-gcc \
    RS64_MIPS_LD=/usr/bin/mips-linux-gnu-ld

ENTRYPOINT ["/usr/local/sbin/rs64-entrypoint.sh"]
