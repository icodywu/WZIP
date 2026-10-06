/*
 * wzip._wzip: the C extension of the Python package wzip (python/wzip/__init__.py has the documented interface)
 * Copyright (c) 2026-present, Yingquan (Cody) Wu.
 * SPDX-License-Identifier: BSD-2-Clause (see LICENSE)
 *
 * Compression and decompression release the GIL, so threads compress in parallel.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "wzframe.h"
#include "WZIP.h"
#include "WLZ4.h"

static PyObject* WzipError;

static PyObject* frame_error(size_t code)
{
	PyErr_SetString(WzipError, WZF_getErrorName(code));
	return NULL;
}

/* compress(data, codec, level, block_log, checksum, threads, window_log) -> bytes: one WZ frame */
static PyObject* py_compress(PyObject* self, PyObject* args)
{
	Py_buffer in;
	int codec, level, blockLog, checksum, threads = 1, windowLog = 0;
	if (!PyArg_ParseTuple(args, "y*iiip|ii", &in, &codec, &level, &blockLog, &checksum, &threads, &windowLog)) return NULL;
	const WZF_params p = { codec, level, blockLog, !checksum, threads, windowLog };
	const size_t bound = WZF_compressBound((size_t)in.len, &p);
	PyObject* out = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)bound);
	if (!out) { PyBuffer_Release(&in); return NULL; }
	size_t r;
	Py_BEGIN_ALLOW_THREADS
	r = WZF_compress(PyBytes_AS_STRING(out), bound, in.buf, (size_t)in.len, &p);
	Py_END_ALLOW_THREADS
	PyBuffer_Release(&in);
	if (WZF_isError(r)) { Py_DECREF(out); return frame_error(r); }
	if (_PyBytes_Resize(&out, (Py_ssize_t)r) < 0) return NULL;
	return out;
}

/* content_size(data) -> int or None */
static PyObject* py_content_size(PyObject* self, PyObject* args)
{
	Py_buffer in;
	if (!PyArg_ParseTuple(args, "y*", &in)) return NULL;
	const unsigned long long n = WZF_getContentSize(in.buf, (size_t)in.len);
	PyBuffer_Release(&in);
	if (n == WZF_CONTENTSIZE_UNKNOWN) Py_RETURN_NONE;
	return PyLong_FromUnsignedLongLong(n);
}

/* decompress(data) -> bytes: every frame, block by block when the content size is not recorded */
static PyObject* py_decompress(PyObject* self, PyObject* args)
{
	Py_buffer in;
	if (!PyArg_ParseTuple(args, "y*", &in)) return NULL;
	const unsigned char* const src = (const unsigned char*)in.buf;
	const size_t srcSize = (size_t)in.len;
	unsigned long long known = WZF_getContentSize(src, srcSize);
	PyObject* out = NULL;
	size_t r = 0;
	if (known != WZF_CONTENTSIZE_UNKNOWN && known <= (unsigned long long)PY_SSIZE_T_MAX) {
		out = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)known);
		if (!out) { PyBuffer_Release(&in); return NULL; }
		Py_BEGIN_ALLOW_THREADS
		r = WZF_decompress(PyBytes_AS_STRING(out), (size_t)known, src, srcSize);
		Py_END_ALLOW_THREADS
		PyBuffer_Release(&in);
		if (WZF_isError(r)) { Py_DECREF(out); return frame_error(r); }
		if (r != known && _PyBytes_Resize(&out, (Py_ssize_t)r) < 0) return NULL;
		return out;
	}
	/* the content size is not recorded (a frame written from a pipe): decode block by block into a growing buffer */
	WZF_DCtx* d = WZF_createDCtx();
	size_t cap = srcSize * 4 + 1024, pos = 0, o = 0;
	char* buf = (char*)PyMem_Malloc(cap);
	if (!d || !buf) { WZF_freeDCtx(d); PyMem_Free(buf); PyBuffer_Release(&in); return PyErr_NoMemory(); }
	while (pos < srcSize) {
		size_t h = WZF_decompressBegin(d, src + pos, srcSize - pos);
		if (WZF_isError(h)) { r = h; break; }
		if (h > srcSize - pos) { r = (size_t)-(ptrdiff_t)WZF_error_srcSize_wrong; break; }
		pos += h;
		if (WZF_frameHeader(d)->skippable) continue;
		for (;;) {
			int raw;
			if (srcSize - pos < WZF_BLOCK_HEADER) { r = (size_t)-(ptrdiff_t)WZF_error_srcSize_wrong; break; }
			const size_t c = WZF_nextBlock(d, src + pos, &raw);
			if (WZF_isError(c)) { r = c; break; }
			pos += WZF_BLOCK_HEADER;
			if (c == 0) break;
			if (srcSize - pos < c) { r = (size_t)-(ptrdiff_t)WZF_error_srcSize_wrong; break; }
			const size_t size = WZF_blockDecodedSize(d, src + pos, c);
			if (WZF_isError(size)) { r = size; break; }
			if (cap - o < size) {
				size_t ncap = cap * 2;
				while (ncap - o < size) ncap *= 2;
				char* nb = (char*)PyMem_Realloc(buf, ncap);
				if (!nb) { r = (size_t)-(ptrdiff_t)WZF_error_memory; break; }
				buf = nb; cap = ncap;
			}
			Py_BEGIN_ALLOW_THREADS
			r = WZF_decompressBlock(d, buf + o, cap - o, src + pos, c);
			Py_END_ALLOW_THREADS
			if (WZF_isError(r)) break;
			o += r; pos += c;
		}
		if (WZF_isError(r)) break;
		const size_t e = WZF_endSize(d);
		if (srcSize - pos < e) { r = (size_t)-(ptrdiff_t)WZF_error_srcSize_wrong; break; }
		r = WZF_decompressEnd(d, src + pos, e);
		if (WZF_isError(r)) break;
		pos += e;
	}
	WZF_freeDCtx(d);
	PyBuffer_Release(&in);
	if (WZF_isError(r)) { PyMem_Free(buf); return frame_error(r); }
	out = PyBytes_FromStringAndSize(buf, (Py_ssize_t)o);
	PyMem_Free(buf);
	return out;
}

/* bare codec streams, without a frame: for applications that store sizes and codecs themselves */
static PyObject* py_wlz4_compress(PyObject* self, PyObject* args)
{
	Py_buffer in;
	int level;
	if (!PyArg_ParseTuple(args, "y*i", &in, &level)) return NULL;
	if (in.len > WLZ_MAX_INPUT_SIZE || level < -2 || level > 12) {
		PyBuffer_Release(&in);
		PyErr_SetString(PyExc_ValueError, "WLZ4: input above 2 GB, or level not in -2..12");
		return NULL;
	}
	const unsigned n = (unsigned)in.len, bound = WLZ_COMPRESSBOUND(n);
	PyObject* out = PyBytes_FromStringAndSize(NULL, bound);
	if (!out) { PyBuffer_Release(&in); return NULL; }
	unsigned r = 0;
	Py_BEGIN_ALLOW_THREADS
	if (level < 0) {
		WLZ_State_Str* s = WLZ_New_State();
		if (s) {
			r = level == -2 ? WLZ_Compress_Fast(s, (const char*)in.buf, PyBytes_AS_STRING(out), n, bound, 1)
			                : WLZ_Compress(s, (const char*)in.buf, PyBytes_AS_STRING(out), n, bound);
			WLZ_Free_State(s);
		}
	}
	else {
		WLZhc_State_Str* s = WLZhc_New_State();
		if (s) {
			r = WLZhc_Compress(s, (const char*)in.buf, PyBytes_AS_STRING(out), n, bound, level);
			WLZhc_Free_State(s);
		}
	}
	Py_END_ALLOW_THREADS
	PyBuffer_Release(&in);
	if (r == 0) { Py_DECREF(out); PyErr_SetString(WzipError, "WLZ4 compression failed"); return NULL; }
	if (_PyBytes_Resize(&out, (Py_ssize_t)r) < 0) return NULL;
	return out;
}

static PyObject* py_wlz4_decompress(PyObject* self, PyObject* args)
{
	Py_buffer in;
	if (!PyArg_ParseTuple(args, "y*", &in)) return NULL;
	if (in.len > 0xFFFFFFFFLL) { PyBuffer_Release(&in); PyErr_SetString(WzipError, "corrupted data"); return NULL; }
	const unsigned n = WLZ_Read_DecSize((const char*)in.buf, (unsigned)in.len);
	const Py_ssize_t len = in.len;
	PyObject* out = PyBytes_FromStringAndSize(NULL, n);
	if (!out) { PyBuffer_Release(&in); return NULL; }
	unsigned r;
	Py_BEGIN_ALLOW_THREADS
	r = WLZ_Decompress((const char*)in.buf, PyBytes_AS_STRING(out), (unsigned)len, n);
	Py_END_ALLOW_THREADS
	PyBuffer_Release(&in);
	if (r != n || len < 4) { Py_DECREF(out); PyErr_SetString(WzipError, "corrupted data"); return NULL; }
	return out;
}

static PyObject* py_wzip_compress(PyObject* self, PyObject* args)
{
	Py_buffer in;
	int level;
	if (!PyArg_ParseTuple(args, "y*i", &in, &level)) return NULL;
	if (in.len > WZIP_MAX_INPUT_SIZE || level < 0 || level > 13) {
		PyBuffer_Release(&in);
		PyErr_SetString(PyExc_ValueError, "WZIP: input above 2 GB, or level not in 0..13");
		return NULL;
	}
	int cap = WZIP_Cap_CmprSize((int)in.len);
	PyObject* out = PyBytes_FromStringAndSize(NULL, cap);
	if (!out) { PyBuffer_Release(&in); return NULL; }
	int r;
	Py_BEGIN_ALLOW_THREADS
	r = wzip_compress(in.buf, (int)in.len, PyBytes_AS_STRING(out), &cap, level);
	Py_END_ALLOW_THREADS
	PyBuffer_Release(&in);
	if (r <= 0) { Py_DECREF(out); PyErr_SetString(WzipError, "WZIP compression failed"); return NULL; }
	if (_PyBytes_Resize(&out, r) < 0) return NULL;
	return out;
}

static PyObject* py_wzip_decompress(PyObject* self, PyObject* args)
{
	Py_buffer in;
	if (!PyArg_ParseTuple(args, "y*", &in)) return NULL;
	if (in.len > 0x7FFFFFFF || in.len < 2) { PyBuffer_Release(&in); PyErr_SetString(WzipError, "corrupted data"); return NULL; }
	int left = (int)in.len;
	const int ds = WZIP_Read_DecSize(in.buf, &left);
	const int n = ds ? ds : left;                       /* 0: stored, the rest of the stream */
	PyObject* out = PyBytes_FromStringAndSize(NULL, n);
	if (!out) { PyBuffer_Release(&in); return NULL; }
	int r, cap = n;
	Py_BEGIN_ALLOW_THREADS
	r = wzip_decompress(in.buf, (int)in.len, PyBytes_AS_STRING(out), &cap);
	Py_END_ALLOW_THREADS
	PyBuffer_Release(&in);
	if (r != n) { Py_DECREF(out); PyErr_SetString(WzipError, "corrupted data"); return NULL; }
	return out;
}

static PyMethodDef methods[] = {
	{ "compress", py_compress, METH_VARARGS, "compress(data, codec, level, block_log, checksum) -> bytes" },
	{ "decompress", py_decompress, METH_VARARGS, "decompress(data) -> bytes" },
	{ "content_size", py_content_size, METH_VARARGS, "content_size(data) -> int or None" },
	{ "wlz4_compress", py_wlz4_compress, METH_VARARGS, "wlz4_compress(data, level) -> bytes (a bare WLZ4 block)" },
	{ "wlz4_decompress", py_wlz4_decompress, METH_VARARGS, "wlz4_decompress(block) -> bytes" },
	{ "wzip_compress", py_wzip_compress, METH_VARARGS, "wzip_compress(data, level) -> bytes (a bare WZIP stream)" },
	{ "wzip_decompress", py_wzip_decompress, METH_VARARGS, "wzip_decompress(stream) -> bytes" },
	{ NULL, NULL, 0, NULL }
};

static struct PyModuleDef module = { PyModuleDef_HEAD_INIT, "_wzip", "WZIP and WLZ4 (C extension)", -1, methods, NULL, NULL, NULL, NULL };

PyMODINIT_FUNC PyInit__wzip(void)
{
	PyObject* m = PyModule_Create(&module);
	if (!m) return NULL;
	WzipError = PyErr_NewException("wzip.Error", NULL, NULL);
	if (!WzipError || PyModule_AddObject(m, "Error", WzipError) < 0) { Py_XDECREF(WzipError); Py_DECREF(m); return NULL; }
	Py_INCREF(WzipError);
	PyModule_AddStringConstant(m, "VERSION", WZF_versionString());
	PyModule_AddIntConstant(m, "CODEC_WZIP", WZF_CODEC_WZIP);
	PyModule_AddIntConstant(m, "CODEC_WLZ4", WZF_CODEC_WLZ4);
	return m;
}
