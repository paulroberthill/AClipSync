# AClipSync - clipboard sync between AmigaOS 3.x and a PC
#
#   make              Amiga daemon (build/amiga/AClipSync) and host unit tests
#   make test         run the C unit tests on the host, then the Python tests
#   make test-amiga   run the C unit tests as 68k code under vamos
#   make dist         build/AClipSync.lha: daemon, README and PC companion
#   make clean

HOST_CC  ?= cc
AMIGA_CC ?= /opt/amiga/bin/m68k-amigaos-gcc
AMIGA_STRIP ?= /opt/amiga/bin/m68k-amigaos-strip
VAMOS    ?= ../ARSync/.venv/bin/vamos
PYTHON   ?= python3

WARN = -Wall -Wextra -Wno-unused-parameter
HOST_CFLAGS = -std=gnu99 -O2 -g $(WARN)
# libnix: small binaries, and its WB startup sets _WBenchMsg. 68000 code, so
# it runs on any Amiga.
AMIGA_RT = -mcrt=nix20
# -Wno-pointer-sign: the NDK's STRPTR is unsigned char *.
AMIGA_CFLAGS = -std=gnu99 -O2 $(AMIGA_RT) $(WARN) -Wno-pointer-sign

CORE_SRC = src/ftxt.c src/frame.c
TEST_SRC = tests/test_core.c

.PHONY: all test test-amiga dist clean

all: build/amiga/AClipSync build/host/test_core

build/amiga/AClipSync: src/main.c $(CORE_SRC) src/ftxt.h src/frame.h | build/amiga
	$(AMIGA_CC) $(AMIGA_CFLAGS) -o $@ src/main.c $(CORE_SRC)
	$(AMIGA_STRIP) $@

build/host/test_core: $(TEST_SRC) $(CORE_SRC) src/ftxt.h src/frame.h | build/host
	$(HOST_CC) $(HOST_CFLAGS) -Isrc -o $@ $(TEST_SRC) $(CORE_SRC)

build/amiga/test_core: $(TEST_SRC) $(CORE_SRC) src/ftxt.h src/frame.h | build/amiga
	$(AMIGA_CC) $(AMIGA_CFLAGS) -Isrc -o $@ $(TEST_SRC) $(CORE_SRC)

test: build/host/test_core
	build/host/test_core
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py'

test-amiga: build/amiga/test_core
	$(VAMOS) build/amiga/test_core

dist: build/amiga/AClipSync
	rm -rf build/dist && mkdir -p build/dist/AClipSync
	cp build/amiga/AClipSync README.md pc/aclipsync.py build/dist/AClipSync/
	cd build/dist && lha -aq ../AClipSync.lha AClipSync

build/amiga build/host:
	mkdir -p $@

clean:
	rm -rf build
