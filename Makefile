# room only builds and runs on Linux. On another system use the docker-*
# targets. "make help" lists every target.
#
# Variables, all optional:
#   CC CFLAGS CPPFLAGS LDFLAGS LDLIBS  as usual. They are added after the
#                     project's own flags, so they extend them
#   PREFIX DESTDIR    where install puts bin/room and install-systemd the unit
#   BUILD             where every build output goes (default build)
#   STATIC=1          a static PIE
#   SAN=1             AddressSanitizer and UndefinedBehaviorSanitizer
# make doesn't notice when only flags change, so give a build with other
# flags its own BUILD, or run make clean first.

CFLAGS ?= -O2
PREFIX ?= /usr/local
BUILD ?= build

GCC ?= gcc
CLANG ?= clang
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
CPPCHECK ?= cppcheck

# gcc and clang build room without any of these. The conversion warnings
# are left out because every narrowing in room is range checked or wanted,
# and -Wcast-qual and -Wwrite-strings because execve() takes the
# environment as char **.
WARNINGS = -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
    -Wold-style-definition -Wvla -Wformat=2 -Wundef -Wpointer-arith -Wnull-dereference

ROOM_CPPFLAGS = -D_GNU_SOURCE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3
ROOM_CFLAGS = -std=gnu11 $(WARNINGS) -fstack-protector-strong -fPIE
ROOM_LDFLAGS = -pie -Wl,-z,relro,-z,now,-z,noexecstack

ifeq ($(STATIC),1)
ROOM_LDFLAGS := $(ROOM_LDFLAGS:-pie=-static-pie)
endif
ifeq ($(SAN),1)
# The sanitizers replace the checks _FORTIFY_SOURCE would add.
ROOM_CPPFLAGS := $(filter-out -D_FORTIFY_SOURCE=3,$(ROOM_CPPFLAGS))
ROOM_CFLAGS += -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
endif

ALL_CPPFLAGS = $(ROOM_CPPFLAGS) $(CPPFLAGS)
ALL_CFLAGS = $(ROOM_CFLAGS) $(CFLAGS)
ALL_LDFLAGS = $(ROOM_LDFLAGS) $(LDFLAGS)

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:src/%.c=$(BUILD)/%.o)
FORMAT_FILES = $(wildcard src/*.c src/*.h tests/*.c)
VERSION = $(shell sed -n 's/.*define ROOM_VERSION "\(.*\)"/\1/p' src/room.h)

all: $(BUILD)/room ## Build room into the build directory (the default)

$(BUILD)/room: $(OBJS)
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/seccomp_sim.o: tests/seccomp_sim.c | $(BUILD)
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/seccomp_sim: $(BUILD)/seccomp_sim.o $(BUILD)/util.o
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(LDLIBS)

# The program the tests run in the box. Static, because a box may have no
# libraries. Always gcc -O2 and none of the flags above: clang removes the
# allocations of "t alloc", which the memory tests depend on.
$(BUILD)/t: tests/t.c | $(BUILD)
	$(GCC) -O2 -Wall -Wextra -static -pthread -o $@ tests/t.c

$(BUILD):
	mkdir -p $@

-include $(OBJS:.o=.d) $(BUILD)/seccomp_sim.d

install: $(BUILD)/room ## Install room into DESTDIR/PREFIX/bin
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD)/room $(DESTDIR)$(PREFIX)/bin/room

install-systemd: ## Install room.service into DESTDIR/PREFIX/lib/systemd/system
	install -d $(DESTDIR)$(PREFIX)/lib/systemd/system
	install -m 644 systemd/room.service $(DESTDIR)$(PREFIX)/lib/systemd/system/room.service

uninstall: ## Remove what install and install-systemd installed
	rm -f $(DESTDIR)$(PREFIX)/bin/room $(DESTDIR)$(PREFIX)/lib/systemd/system/room.service

clean: ## Remove the build directory
	rm -rf $(BUILD)

# With SAN=1 the sanitizers write their reports into BUILD/san, and any
# report fails the target. Build with clang: next to ASan, gcc's UBSan
# ignores log_path and reports only to room's stderr, which the suite hides.
ifeq ($(SAN),1)
SAN_LOG = $(abspath $(BUILD))/san
test: export ASAN_OPTIONS = detect_leaks=0:log_path=$(SAN_LOG)/asan
test: export UBSAN_OPTIONS = print_stacktrace=1:log_path=$(SAN_LOG)/ubsan
endif

test: $(BUILD)/room $(BUILD)/t $(BUILD)/seccomp_sim ## Run the suite. Needs root on Linux with CAP_SYS_ADMIN
ifeq ($(SAN),1)
	rm -rf $(SAN_LOG)
	mkdir -p $(SAN_LOG)
endif
	ROOM_TEST_BUILD=$(abspath $(BUILD)) sh tests/run.sh $(BUILD)/room
ifeq ($(SAN),1)
	@if [ -n "$$(ls $(SAN_LOG))" ]; then cat $(SAN_LOG)/*; exit 1; fi
	@echo "No sanitizer reports"
endif

format: ## Format the C code with clang-format
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check: ## Fail if the C code isn't formatted
	$(CLANG_FORMAT) --dry-run -Werror $(FORMAT_FILES)

# cppcheck reads no system headers, so it needs the architecture. x86_64 is
# the one with the most code. readdirCalled wants readdir_r(), which glibc
# deprecates: readdir() is safe on a stream only one thread uses.
lint: ## Run clang-tidy, cppcheck and gcc -fanalyzer
	$(CLANG_TIDY) --quiet $(SRCS) -- $(ROOM_CPPFLAGS) -std=gnu11
	$(CPPCHECK) --quiet --error-exitcode=1 --check-level=exhaustive --std=c11 \
	    --enable=warning,portability,performance --library=posix --suppress=readdirCalled \
	    $(ROOM_CPPFLAGS) -D__x86_64__ src
	for f in $(SRCS); do \
	    $(GCC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) -fanalyzer -Werror -c -o /dev/null $$f || exit 1; \
	done

check: format-check lint ## Format check, lint, -Werror builds with gcc and clang, filter simulator
	$(MAKE) BUILD=$(BUILD)/gcc CC=$(GCC) CFLAGS="$(CFLAGS) -Werror" all $(BUILD)/gcc/seccomp_sim
	$(MAKE) BUILD=$(BUILD)/clang CC=$(CLANG) CFLAGS="$(CFLAGS) -Werror" all
	$(BUILD)/gcc/seccomp_sim

# For linux/amd64 and linux/arm64, with docker.
dist: ## Build the static release binaries and SHA256SUMS into BUILD/dist
	rm -rf $(BUILD)/dist
	docker build --platform linux/amd64,linux/arm64 --target release --output $(BUILD)/dist .
	mv $(BUILD)/dist/linux_amd64/room $(BUILD)/dist/room-$(VERSION)-linux-amd64
	mv $(BUILD)/dist/linux_arm64/room $(BUILD)/dist/room-$(VERSION)-linux-arm64
	rmdir $(BUILD)/dist/linux_amd64 $(BUILD)/dist/linux_arm64
	cd $(BUILD)/dist && sha256sum room-* > SHA256SUMS

image: ## Build the release image room-release for this machine
	docker build --target release -t room-release .

TOOLS_IMAGE = docker build -q --target tools -t room-tools .
TOOLS = docker run --rm -w /usr/src/room

# Docker's default AppArmor profile denies every mount, so on a host with
# AppArmor SYS_ADMIN alone isn't enough. Docker ignores the option elsewhere.
SYS_ADMIN = --cap-add SYS_ADMIN --security-opt apparmor=unconfined

docker-test: ## Run the suite in Docker, with SYS_ADMIN and with --privileged
	docker build -t room-test .
	docker run --rm $(SYS_ADMIN) room-test
	docker run --rm --privileged room-test

# Systemd as PID 1 with room.service enabled, standing in for a machine
# without Docker. The container goes away however the suite ends.
docker-test-systemd: ## Run the suite in Docker under systemd, with room.service
	docker build --target systemd -t room-test-systemd .
	c=$$(docker run -d --privileged --cgroupns=private --tmpfs /run --tmpfs /run/lock \
	    room-test-systemd) || exit 2; \
	trap 'docker rm -f "$$c" > /dev/null' EXIT; \
	trap 'exit 130' INT TERM HUP; \
	i=0; \
	until docker exec "$$c" systemctl is-active -q room.service; do \
	    i=$$((i + 1)); \
	    if [ $$i -gt 60 ]; then \
	        docker exec "$$c" systemctl status room.service; \
	        echo "room.service is not active after 30 seconds" >&2; \
	        exit 2; \
	    fi; \
	    sleep 0.5; \
	done; \
	docker exec "$$c" sh tests/run.sh /usr/local/bin/room

SAN_TEST = -v "$(CURDIR):/usr/src/room:ro" room-tools make test SAN=1 CC=clang-19 BUILD=/tmp/san

docker-test-san: ## Like docker-test, against a build with ASan and UBSan
	$(TOOLS_IMAGE)
	$(TOOLS) $(SYS_ADMIN) $(SAN_TEST)
	$(TOOLS) --privileged $(SAN_TEST)

docker-check: ## Run check in the tools image, on a read-only mount of the repository
	$(TOOLS_IMAGE)
	$(TOOLS) --user "$$(id -u):$$(id -g)" -v "$(CURDIR):/usr/src/room:ro" room-tools \
	    make check BUILD=/tmp/build

docker-format: ## Run format in the tools image
	$(TOOLS_IMAGE)
	$(TOOLS) --user "$$(id -u):$$(id -g)" -v "$(CURDIR):/usr/src/room" room-tools make format

help: ## List the targets
	@awk -F ':.*## ' '/^[a-z-]+:.*## / { printf "  %-20s %s\n", $$1, $$2 }' $(firstword $(MAKEFILE_LIST))

.PHONY: all install install-systemd uninstall clean test format format-check lint check dist \
    image help docker-test docker-test-systemd docker-test-san docker-check docker-format
