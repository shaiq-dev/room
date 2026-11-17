# Stage 1: a static, stripped room built on Alpine (musl).
FROM alpine:3.22 AS build
RUN apk add --no-cache build-base linux-headers
WORKDIR /usr/src/room
COPY . .
RUN make STATIC=1 LDFLAGS=-s

# Stage 2: the release image. It holds nothing but /room.
#   docker build --target release -t room-release .
# Use it from another Dockerfile with
#   COPY --from=room-release /room /usr/local/bin/room
FROM scratch AS release
COPY --from=build /usr/src/room/build/room /room

# Stage 3: what make docker-check, docker-format and docker-test-san need.
# The repository is mounted, not copied:
#   make docker-check
# The tool versions are fixed by the Debian release, so everyone formats
# and lints with the same ones.
FROM debian:trixie-slim AS tools
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        clang-19 clang-format-19 clang-tidy-19 cppcheck gcc libc6-dev libclang-rt-19-dev make \
        procps util-linux \
    && rm -rf /var/lib/apt/lists/*
ENV CLANG=clang-19 CLANG_FORMAT=clang-format-19 CLANG_TIDY=clang-tidy-19

# Stage 4: the test image (Debian, glibc). The last stage repeats it as the default.
#   docker build -t room-test .
#   docker run --rm --cap-add SYS_ADMIN room-test
FROM debian:trixie-slim AS test
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev make procps util-linux \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /usr/src/room
COPY . .
# A compile check with glibc, and the helpers the suite uses.
RUN make CFLAGS="-O2 -Werror" all build/t build/seccomp_sim
# The suite tests the exact binary that is released.
COPY --from=release /room /usr/local/bin/room
CMD ["sh", "tests/run.sh", "/usr/local/bin/room"]

# Stage 5: the test image booting systemd, with room.service installed and
# enabled, for make docker-test-systemd. The suite runs in it with docker exec.
FROM test AS systemd
RUN apt-get update \
    && apt-get install -y --no-install-recommends systemd \
    && rm -rf /var/lib/apt/lists/*
RUN make install-systemd && systemctl enable room.service
# Debian puts /tmp on a nodev tmpfs, where the suite's scratch directory
# could hold no device nodes and its files couldn't be hard-linked into a box.
RUN systemctl mask tmp.mount
STOPSIGNAL SIGRTMIN+3
CMD ["/lib/systemd/systemd"]

# The default, so a build without --target gives the test image.
FROM test
