# Benchmark harness of the paper. It uses Windows APIs (thread affinity, peak working set) and was built with
# MSYS2 MinGW-w64: LZ4 1.10.0 and Zstandard 1.5.6 from source (make deps), Brotli and liblzma from MSYS2
# (pacman -S mingw-w64-x86_64-brotli mingw-w64-x86_64-xz).
#   make deps     clones LZ4 and Zstandard at the benchmarked versions into deps/
#   make          builds bench_all.exe; on Linux make deps deps-linux, then make bench_all bench_blocks (see below)

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

all: bench_all.exe bench_blocks.exe

bench_all.exe: bench_all.c $(WZIP_SRC) $(LZ4_SRC)
	$(CC) $(CFLAGS) -w -I../src -I$(LZ4) -I$(ZSTD) -DZSTD_DISABLE_ASM bench_all.c $(WZIP_SRC) $(LZ4_SRC) $(ZSTD_SRC) \
		-lbrotlienc -lbrotlidec -llzma -lpsapi -lm -o $@

# Linux: the same harness (pinned by env BENCH_CPU or by taskset), with Zstandard's assembly Huffman decoder as its
# Linux builds use by default; Brotli and liblzma from the system, or built from source by deps-linux
# (make bench_all EXTRA_INC=-Ideps/prefix/include EXTRA_LIB=-Ldeps/prefix/lib)
bench_all: bench_all.c $(WZIP_SRC) $(LZ4_SRC)
	$(CC) $(CFLAGS) -w -I../src -I$(LZ4) -I$(ZSTD) $(EXTRA_INC) bench_all.c $(WZIP_SRC) $(LZ4_SRC) $(ZSTD_SRC) \
		$(ZSTD)/decompress/huf_decompress_amd64.S $(EXTRA_LIB) -lbrotlienc -lbrotlidec -lbrotlicommon -llzma -lm -o $@

bench_blocks: bench_blocks.c $(WZIP_SRC) $(LZ4_SRC)
	$(CC) $(CFLAGS) -w -I../src -I$(LZ4) -I$(ZSTD) -DS_W2_BITS=$(S_W2) bench_blocks.c $(WZIP_SRC) $(LZ4_SRC) $(ZSTD_SRC) \
		$(ZSTD)/decompress/huf_decompress_amd64.S -lm -o $@

# Brotli 1.1.0 and xz 5.6.2 from source, static, installed into deps/prefix
deps-linux:
	git clone --depth 1 --branch v1.1.0 https://github.com/google/brotli $(DEPS)/brotli
	git clone --depth 1 --branch v5.6.2 https://github.com/tukaani-project/xz $(DEPS)/xz
	cmake -S $(DEPS)/brotli -B $(DEPS)/brotli/out -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
		-DCMAKE_INSTALL_PREFIX=$(abspath $(DEPS))/prefix
	cmake --build $(DEPS)/brotli/out -j 16 && cmake --install $(DEPS)/brotli/out
	cmake -S $(DEPS)/xz -B $(DEPS)/xz/out -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
		-DCMAKE_INSTALL_PREFIX=$(abspath $(DEPS))/prefix
	cmake --build $(DEPS)/xz/out -j 16 && cmake --install $(DEPS)/xz/out

# independent blocks (storage pages); S_W2 builds a WZIP_S that also codes length-2 matches with S_W2 raw offset bits
S_W2 ?= 0
bench_blocks.exe: bench_blocks.c $(WZIP_SRC) $(LZ4_SRC)
	$(CC) $(CFLAGS) -w -I../src -I$(LZ4) -I$(ZSTD) -DZSTD_DISABLE_ASM -DS_W2_BITS=$(S_W2) bench_blocks.c $(WZIP_SRC) \
		$(LZ4_SRC) $(ZSTD_SRC) -lm -o $@

deps:
	git clone --depth 1 --branch v1.10.0 https://github.com/lz4/lz4 $(DEPS)/lz4
	git clone --depth 1 --branch v1.5.6 https://github.com/facebook/zstd $(DEPS)/zstd

clean:
	rm -f bench_all.exe bench_blocks.exe bench_all bench_blocks

.PHONY: deps deps-linux clean
