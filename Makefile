# WZIP and WLZ4: static library and round-trip test (GNU make; gcc or clang)
#   make          builds build/libwzip.a and build/roundtrip
#   make test     runs the round-trip test on built-in synthetic inputs
#   make check FILES="a b c"   runs it on your files
# The benchmark harness of the paper has its own Makefile in bench/.

ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS ?= -O2
CFLAGS += -Wall -Isrc
AR     ?= ar
EXE    := $(if $(filter Windows_NT,$(OS)),.exe,)

SRC := src/WZIP_L.c src/WZIP_M.c src/WZIP_S.c src/WZIP_wrapper.c src/Huffman_Compress.c src/Huffman_Decompress.c src/WLZ4.c
OBJ := $(SRC:src/%.c=build/%.o)
HDR := $(wildcard src/*.h)

all: build/libwzip.a build/roundtrip$(EXE)

build:
	mkdir -p build

build/%.o: src/%.c $(HDR) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/libwzip.a: $(OBJ)
	$(AR) rcs $@ $^

build/roundtrip$(EXE): tests/roundtrip.c build/libwzip.a
	$(CC) $(CFLAGS) $< build/libwzip.a -lm -o $@

test: build/roundtrip$(EXE)
	./build/roundtrip$(EXE)

check: build/roundtrip$(EXE)
	./build/roundtrip$(EXE) $(FILES)

clean:
	rm -rf build

.PHONY: all test check clean
