# Benchmark harness of the paper. It uses Windows APIs (thread affinity, peak working set) and was built with
# MSYS2 MinGW-w64: LZ4 1.10.0 and Zstandard 1.5.6 from source (make deps), Brotli and liblzma from MSYS2
# (pacman -S mingw-w64-x86_64-brotli mingw-w64-x86_64-xz).
#   make deps     clones LZ4 and Zstandard at the benchmarked versions into deps/
#   make          builds bench_all.exe

ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS ?= -O2
DEPS   ?= deps
LZ4    := $(DEPS)/lz4/lib
ZSTD   := $(DEPS)/zstd/lib

WZIP_SRC := $(wildcard ../src/*.c)
LZ4_SRC  := $(LZ4)/lz4.c $(LZ4)/lz4hc.c
ZSTD_SRC  = $(wildcard $(ZSTD)/common/*.c $(ZSTD)/compress/*.c $(ZSTD)/decompress/*.c)

bench_all.exe: bench_all.c $(WZIP_SRC) $(LZ4_SRC)
	$(CC) $(CFLAGS) -w -I../src -I$(LZ4) -I$(ZSTD) -DZSTD_DISABLE_ASM bench_all.c $(WZIP_SRC) $(LZ4_SRC) $(ZSTD_SRC) \
		-lbrotlienc -lbrotlidec -llzma -lpsapi -lm -o $@

deps:
	git clone --depth 1 --branch v1.10.0 https://github.com/lz4/lz4 $(DEPS)/lz4
	git clone --depth 1 --branch v1.5.6 https://github.com/facebook/zstd $(DEPS)/zstd

clean:
	rm -f bench_all.exe

.PHONY: deps clean
