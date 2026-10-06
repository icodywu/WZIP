# Builds the Python package wzip (pyproject.toml holds its metadata): pip install .
import sys
from setuptools import setup, Extension

sources = ["python/_wzip.c"] + ["src/" + name for name in (
    "WZIP_L.c", "WZIP_M.c", "WZIP_S.c", "WZIP_wrapper.c", "Huffman_Compress.c", "Huffman_Decompress.c",
    "WLZ4.c", "wzframe.c")]
setup(ext_modules=[Extension("wzip._wzip", sources=sources, include_dirs=["src"],
                             libraries=[] if sys.platform == "win32" else ["m"])])
