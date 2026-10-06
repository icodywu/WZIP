"""WZIP and WLZ4 compression.

WZIP is an entropy-coded (Huffman) LZ77 codec of the Zstandard class; WLZ4 a byte-aligned one of the LZ4 class, which
decodes several times as fast. Both use match-length-dependent windows (https://github.com/icodywu/WZIP).

    import wzip
    packed = wzip.compress(data)                    # WZIP, level 1, in a WZ frame with an XXH32 checksum
    packed = wzip.compress(data, "wlz4", level=10)  # WLZ4
    data = wzip.decompress(packed)                  # either codec; raises wzip.Error on damaged input

compress() writes the WZ frame of doc/frame_format.md, the format of the wzip and wlz4 command-line tools, so files
are interchangeable. The bare codec streams (no frame, no checksum) are available as wzip_compress()/wzip_decompress()
and wlz4_compress()/wlz4_decompress(). Calls release the GIL: threads compress and decompress in parallel.
"""
from . import _wzip

__all__ = ["compress", "decompress", "content_size", "open", "Error", "__version__",
           "wzip_compress", "wzip_decompress", "wlz4_compress", "wlz4_decompress"]

__version__ = _wzip.VERSION
Error = _wzip.Error

_CODECS = {"wzip": _wzip.CODEC_WZIP, "wlz4": _wzip.CODEC_WLZ4}
_WLZ4_MODES = {"fast": -2, "lazy": -1}


def _level(codec, level):
    if codec == "wlz4":
        if level is None:
            return -1
        if isinstance(level, str):
            if level not in _WLZ4_MODES:
                raise ValueError("WLZ4 levels are 0-12, 'fast' and 'lazy'")
            return _WLZ4_MODES[level]
        if not 0 <= level <= 12:
            raise ValueError("WLZ4 levels are 0-12, 'fast' and 'lazy'")
        return level
    if level is None:
        return 1
    if not isinstance(level, int) or not 0 <= level <= 13:
        raise ValueError("WZIP levels are 0-13")
    return level


def compress(data, codec="wzip", level=None, block_log=0, checksum=True):
    """Compresses bytes-like data into one WZ frame.

    codec: "wzip" (levels 0-13, default 1; 7-13 optimal parsing) or "wlz4" (levels 0-12, or "fast", "lazy", the
    default). block_log: blocks of at most 2**block_log bytes (10-31; 0: the whole input up to 1 GiB, one block).
    checksum: append an XXH32 checksum of the content."""
    if codec not in _CODECS:
        raise ValueError("codec must be 'wzip' or 'wlz4'")
    if block_log and not 10 <= block_log <= 31:
        raise ValueError("block_log must be 0 or 10-31")
    return _wzip.compress(data, _CODECS[codec], _level(codec, level), block_log, checksum)


def decompress(data):
    """Decompresses every WZ frame in data (skipping skippable frames); raises wzip.Error if data is damaged."""
    return _wzip.decompress(data)


def content_size(data):
    """The content size recorded in the frames of data, or None if a frame does not record it."""
    return _wzip.content_size(data)


def wzip_compress(data, level=1):
    """A bare WZIP stream (no frame, no checksum), as wzip_compress() in C; at most 2 GB."""
    return _wzip.wzip_compress(data, _level("wzip", level))


def wzip_decompress(stream):
    return _wzip.wzip_decompress(stream)


def wlz4_compress(data, level="lazy"):
    """A bare WLZ4 block (no frame, no checksum); at most 2 GB."""
    return _wzip.wlz4_compress(data, _level("wlz4", level))


def wlz4_decompress(block):
    return _wzip.wlz4_decompress(block)


def open(filename, mode="rb", codec="wzip", level=None):
    """Reads or writes a whole .wz or .wlz4 file: with mode "rb" returns its decompressed content; with "wb" returns
    an object whose write() collects data and whose close() (or the end of a with block) compresses it."""
    if mode == "rb":
        import builtins
        with builtins.open(filename, "rb") as f:
            return decompress(f.read())
    if mode == "wb":
        return _Writer(filename, codec, level)
    raise ValueError("mode must be 'rb' or 'wb'")


class _Writer:
    def __init__(self, filename, codec, level):
        self._name, self._codec, self._level, self._parts = filename, codec, level, []

    def write(self, data):
        self._parts.append(bytes(data))
        return len(data)

    def close(self):
        if self._parts is None:
            return
        import builtins
        with builtins.open(self._name, "wb") as f:
            f.write(compress(b"".join(self._parts), self._codec, self._level))
        self._parts = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if exc[0] is None:
            self.close()
        return False
