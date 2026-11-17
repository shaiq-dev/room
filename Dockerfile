# A static, stripped room built with musl.
FROM alpine:3.22 AS build
RUN apk add --no-cache build-base linux-headers
WORKDIR /usr/src/room
COPY . .
RUN make STATIC=1 LDFLAGS=-s

# The release image. It holds only /room.
FROM scratch AS release
COPY --from=build /usr/src/room/build/room /room

# The release files for make dist. fpm only packs files, so this stage runs
# on the build machine's architecture.
FROM --platform=$BUILDPLATFORM ruby:3.4-slim AS packages
RUN apt-get update \
    && apt-get install -y --no-install-recommends binutils rpm \
    && rm -rf /var/lib/apt/lists/* \
    && gem install --no-document fpm -v 1.18.0
ARG TARGETARCH
WORKDIR /usr/src/room
COPY LICENSE ./
COPY src/room.h src/
COPY systemd systemd
COPY packaging packaging
COPY scripts scripts
COPY --from=build /usr/src/room/build/room build/room
RUN sh scripts/build-release.sh "$TARGETARCH" build/room /dist

FROM scratch AS dist
COPY --from=packages /dist/ /

# The tools for make docker-check, docker-format and docker-test-san, which
# mount the repository. Debian fixes the tool versions, so everyone formats
# and lints the same way.
FROM debian:trixie-slim AS tools
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        clang-19 clang-format-19 clang-tidy-19 cppcheck gcc libc6-dev libclang-rt-19-dev make \
        procps util-linux \
    && rm -rf /var/lib/apt/lists/*
ENV CLANG=clang-19 CLANG_FORMAT=clang-format-19 CLANG_TIDY=clang-tidy-19

# The test image, with glibc.
FROM debian:trixie-slim AS test
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev make procps util-linux \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /usr/src/room
COPY . .
# A compile check with glibc, and the suite's helpers.
RUN make CFLAGS="-O2 -Werror" all build/t build/seccomp_sim
# The suite tests the binary that is released.
COPY --from=release /room /usr/local/bin/room
CMD ["sh", "tests/run.sh", "/usr/local/bin/room"]

# The test image with systemd, for make docker-test-systemd.
FROM test AS systemd
RUN apt-get update \
    && apt-get install -y --no-install-recommends systemd \
    && rm -rf /var/lib/apt/lists/*
# The package starts the unit only under a running systemd.
COPY --from=packages /dist/*.deb /tmp/
RUN dpkg -i /tmp/room_*.deb && systemctl enable room.service
# Debian puts /tmp on a nodev tmpfs. The suite needs device nodes and hard
# links there.
RUN systemctl mask tmp.mount
STOPSIGNAL SIGRTMIN+3
CMD ["/lib/systemd/systemd"]

# A build without --target gives the test image.
FROM test
