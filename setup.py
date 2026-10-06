# Builds the Python package wzip (pyproject.toml holds its metadata): pip install .
import sys
from setuptools import setup, Extension

sources = ["python/_wzip.c"] + ["src/" + name for name in (
    "WZIP_L.c", "WZIP_M.c", "WZIP_S.c", "WZIP_wrapper.c", "Huffman_Compress.c", "Huffman_Decompress.c",
    "WLZ4.c", "wzframe.c")]
win = sys.platform == "win32"
setup(ext_modules=[Extension("wzip._wzip", sources=sources, include_dirs=["src"],
                             define_macros=[("WZIP_MULTITHREAD", "1")],          # compress(..., threads=)
                             extra_compile_args=[] if win else ["-pthread"],
                             extra_link_args=[] if win else ["-pthread"],
                             libraries=[] if win else ["m"])])
