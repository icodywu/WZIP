# WZIP and WLZ4: static library, command-line tool and tests (GNU make; gcc or clang)
#   make          builds build/libwzip.a, the tool build/wzip (and its copy build/wlz4), and the tests
#   make test     runs the round-trip test on built-in synthetic inputs, the thread-safety test and the tool's test
#   make check FILES="a b c"   runs it on your files
# The benchmark harness of the paper has its own Makefile in bench/.

ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS ?= -O2
override CFLAGS += -Wall -Isrc
# threads in compression (WZIP_Set_Workers, wzip -T): pthreads, or Win32 threads on Windows; make MT=0 leaves them out
MT ?= 1
ifeq ($(MT),1)
override CFLAGS += -DWZIP_MULTITHREAD=1
ifneq ($(OS),Windows_NT)
override CFLAGS += -pthread
endif
endif
AR     ?= ar
EXE    := $(if $(filter Windows_NT,$(OS)),.exe,)

SRC := src/WZIP_L.c src/WZIP_M.c src/WZIP_S.c src/WZIP_wrapper.c src/Huffman_Compress.c src/Huffman_Decompress.c src/WLZ4.c src/wzframe.c
OBJ := $(SRC:src/%.c=build/%.o)
HDR := $(wildcard src/*.h)

all: build/libwzip.a build/wzip$(EXE) build/wlz4$(EXE) build/roundtrip$(EXE) build/threads$(EXE)

build:
	mkdir -p build

build/%.o: src/%.c $(HDR) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/libwzip.a: $(OBJ)
	$(AR) rcs $@ $^

build/roundtrip$(EXE): tests/roundtrip.c build/libwzip.a
	$(CC) $(CFLAGS) $< build/libwzip.a -lm -o $@

# the same, with every WZIP_L block decoded as a pipeline (these inputs are too small for the decoder to choose it),
# and with windows of at most 2^17 bytes (a test format), so that these inputs wrap the match finder's tree as enwik9
# does, where the threads that split it must not get in each other's way
build/roundtrip_pipe$(EXE): tests/roundtrip.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) -DWZL_PIPELINE=1 -DWZIP_TEST_MAX_OFF_WIDTH=17 tests/roundtrip.c $(SRC) -lm -o $@

build/wzip$(EXE): programs/wzip.c build/libwzip.a
	$(CC) $(CFLAGS) $< build/libwzip.a -lm -o $@

build/wlz4$(EXE): build/wzip$(EXE)
	cp $< $@

build/threads$(EXE): tests/threads.c build/libwzip.a
	$(CC) $(CFLAGS) -pthread $< build/libwzip.a -lm -o $@

test: build/roundtrip$(EXE) build/roundtrip_pipe$(EXE) build/threads$(EXE) build/wzip$(EXE) build/wlz4$(EXE)
	./build/roundtrip$(EXE)
	./build/roundtrip_pipe$(EXE)
	./build/threads$(EXE)
	sh tests/cli.sh build
	sh tests/golden.sh build

check: build/roundtrip$(EXE)
	./build/roundtrip$(EXE) $(FILES)

# decodes damaged streams under AddressSanitizer and UndefinedBehaviorSanitizer (GCC or clang, Linux or macOS)
FUZZFLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Isrc
build/corrupt$(EXE): tests/corrupt.c $(SRC) $(HDR) | build
	$(CC) $(FUZZFLAGS) tests/corrupt.c $(SRC) -lm -o $@
build/corrupt_pipe$(EXE): tests/corrupt.c $(SRC) $(HDR) | build
	$(CC) $(FUZZFLAGS) -DWZL_PIPELINE=1 tests/corrupt.c $(SRC) -lm -o $@

fuzz: build/corrupt$(EXE) build/corrupt_pipe$(EXE)
	./build/corrupt$(EXE) $(ITERS)
	./build/corrupt_pipe$(EXE) $(ITERS)

clean:
	rm -rf build

.PHONY: all test check fuzz clean
