CC ?= cc
QCC ?= qcc
QNX_BUILD ?= build/qnx-x86_64
LINUX_BUILD ?= build/linux
WARNINGS = -Wall -Wextra -Wpedantic
QNX_FLAGS = -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE $(WARNINGS)
LINUX_FLAGS = -std=c11 -D_GNU_SOURCE $(WARNINGS)

.PHONY: qnx portable test
qnx: $(QNX_BUILD)/cuekeeper-engine $(QNX_BUILD)/cuekeeperctl

portable: $(LINUX_BUILD)/cuekeeper-engine $(LINUX_BUILD)/cuekeeperctl

test: portable
	sh tests/smoke.sh "$(LINUX_BUILD)"

$(QNX_BUILD)/cuekeeper-engine: src/engine_qnx.c src/scene.c src/cuekeeper.h
	mkdir -p "$(@D)"
	$(QCC) $(QNX_FLAGS) $(CPPFLAGS) $(CFLAGS) -o "$@" src/engine_qnx.c src/scene.c $(LDFLAGS)

$(QNX_BUILD)/cuekeeperctl: src/controller.c src/cuekeeper.h
	mkdir -p "$(@D)"
	$(QCC) $(QNX_FLAGS) $(CPPFLAGS) $(CFLAGS) -o "$@" src/controller.c $(LDFLAGS)

$(LINUX_BUILD)/cuekeeper-engine: src/engine_linux.c src/scene.c src/cuekeeper.h src/linux_socket.h
	mkdir -p "$(@D)"
	$(CC) $(LINUX_FLAGS) $(CPPFLAGS) $(CFLAGS) -o "$@" src/engine_linux.c src/scene.c $(LDFLAGS)

$(LINUX_BUILD)/cuekeeperctl: src/controller.c src/cuekeeper.h src/linux_socket.h
	mkdir -p "$(@D)"
	$(CC) $(LINUX_FLAGS) $(CPPFLAGS) $(CFLAGS) -o "$@" src/controller.c $(LDFLAGS)
