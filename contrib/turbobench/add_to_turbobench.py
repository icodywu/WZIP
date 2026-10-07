"""Adds WLZ4 (wlz4, wlz4fast, wlz4hc) and WZIP (wzip) to a TurboBench checkout: python add_to_turbobench.py path/to/TurboBench

TurboBench builds each codec from a git submodule; add this repository as the submodule wzip first
(git submodule add https://github.com/icodywu/WZIP wzip). This script then edits plugin.cc (codec ids, headers,
registry, compression, decompression, versions, and freeing WLZ4's tables) and the makefile (the build rule), keeping
each file's line endings (TurboBench keeps plugin.cc in CRLF). Made against TurboBench d12f18a (2026-10-07)."""
import os
import sys

T = sys.argv[1]


def edit(name, pairs):
    p = os.path.join(T, name)
    s = open(p, encoding='utf-8', newline='').read()
    crlf = '\r\n' in s
    s = s.replace('\r\n', '\n')
    if 'P_WZIP' in s or 'D_WZIP' in s:
        sys.exit('%s: wzip is in it already' % name)
    for old, new in pairs:
        if s.count(old) != 1:
            sys.exit('%s: %d matches for %r (TurboBench changed: merge by hand)' % (name, s.count(old), old[:60]))
        s = s.replace(old, new)
    if crlf:
        s = s.replace('\n', '\r\n')
    open(p, 'w', encoding='utf-8', newline='').write(s)


edit('plugin.cc', [
    # the codec ids
    ("#ifndef _WFLZ\n#define _WFLZ 0\n#endif\n P_WFLZ,\n",
     "#ifndef _WFLZ\n#define _WFLZ 0\n#endif\n P_WFLZ,\n \n"
     "#ifndef _WZIP\n#define _WZIP 0\n#endif\n P_WLZ4,\n P_WLZ4FAST,\n P_WLZ4HC,\n P_WZIP,\n"),
    # the headers, and WLZ4's tables (made at the first compression, freed by codexit)
    ("  #if _WFLZ\n#include \"wflz/wfLZ.h\"\n  #endif\n",
     "  #if _WFLZ\n#include \"wflz/wfLZ.h\"\n  #endif\n\n"
     "  #if _WZIP\n#include \"wzip/src/WLZ4.h\"\n#include \"wzip/src/WZIP.h\"\n"
     "static WLZ_State_Str *wlz4st;\n"
     "static WLZhc_State_Str *wlz4hcst;\n  #endif\n"),
    # the registry
    ("  { P_WFLZ,          \"wflz\",          _WFLZ,      \"wfLZ\",                        \"1,2\" },\n",
     "  { P_WFLZ,          \"wflz\",          _WFLZ,      \"wfLZ\",                        \"1,2\" },\n"
     "  { P_WLZ4,          \"wlz4\",          _WZIP,      \"wlz4\",                        \"\" },\n"
     "  { P_WLZ4FAST,      \"wlz4fast\",      _WZIP,      \"wlz4\",                        \"1,2,3,4,5,6,7,8,9,10,12,16,20,24,32,48,64,99\" },\n"
     "  { P_WLZ4HC,        \"wlz4hc\",        _WZIP,      \"wlz4\",                        \"0,1,2,3,4,5,6,7,8,9,10,11,12\" },\n"
     "  { P_WZIP,          \"wzip\",          _WZIP,      \"wzip\",                        \"0,1,2,3,4,5,6,7,8,9,10,11,12,13/t#\" },\n"),
    # compression; wzip's levels 7-13 find matches in up to t threads, with the output of one
    ("(const uint8_t* WF_RESTRICT)workmem, 0 );\n      #endif\n",
     "(const uint8_t* WF_RESTRICT)workmem, 0 );\n      #endif\n\n"
     "      #if _WZIP\n"
     "    case P_WLZ4:     if(!wlz4st && !(wlz4st = WLZ_New_State())) return 0;\n"
     "                     return WLZ_Compress(wlz4st, (const char *)in, (char *)out, inlen, outsize);\n"
     "    case P_WLZ4FAST: if(!wlz4st && !(wlz4st = WLZ_New_State())) return 0;\n"
     "                     return WLZ_Compress_Fast(wlz4st, (const char *)in, (char *)out, inlen, outsize, lev);\n"
     "    case P_WLZ4HC:   if(!wlz4hcst && !(wlz4hcst = WLZhc_New_State())) return 0;\n"
     "                     return WLZhc_Compress(wlz4hcst, (const char *)in, (char *)out, inlen, outsize, lev);\n"
     "    case P_WZIP:     { int cap = outsize > 0x7fffffffu ? 0x7fffffff : (int)outsize;\n"
     "                       int rc = wzip_compress_mt(in, (int)inlen, out, &cap, lev, threads); return rc > 0 ? rc : 0; }\n"
     "      #endif\n"),
    # decompression, with the bounds-checked decoders
    ("    case P_WFLZ:    wfLZ_Decompress( in, out); return inlen;\n      #endif\n",
     "    case P_WFLZ:    wfLZ_Decompress( in, out); return inlen;\n      #endif\n\n"
     "      #if _WZIP\n"
     "    case P_WLZ4: case P_WLZ4FAST: case P_WLZ4HC:\n"
     "      return WLZ_Decompress((const char *)in, (char *)out, inlen, outlen) == outlen ? inlen : 0;\n"
     "    case P_WZIP: { int cap = (int)outlen; return wzip_decompress(in, (int)inlen, out, &cap) == (int)outlen ? inlen : 0; }\n"
     "      #endif\n"),
    # the versions
    ("char *codver(int codec, char *v, char *s) {\n  switch(codec) { \n",
     "char *codver(int codec, char *v, char *s) {\n  switch(codec) { \n"
     "      #if _WZIP\n"
     "    case P_WLZ4: case P_WLZ4FAST: case P_WLZ4HC: sprintf(s, \"v%d.%d.%d\", WLZ_VERSION_MAJOR, WLZ_VERSION_MINOR, WLZ_VERSION_RELEASE); break;\n"
     "    case P_WZIP:     sprintf(s, \"v%s MT\", WZIP_VERSION_STRING); break;\n"
     "      #endif\n"),
    # WLZ4's tables freed after each codec
    ("      #if _SNAPPY_C\n    case P_SNAPPY_C: snappy_free_env(&env);\n",
     "      #if _WZIP\n"
     "    case P_WLZ4: case P_WLZ4FAST: if(wlz4st) WLZ_Free_State(wlz4st); wlz4st = NULL; break;\n"
     "    case P_WLZ4HC:   if(wlz4hcst) WLZhc_Free_State(wlz4hcst); wlz4hcst = NULL; break;\n"
     "      #endif\n"
     "      #if _SNAPPY_C\n    case P_SNAPPY_C: snappy_free_env(&env);\n"),
])

edit('makefile', [
    ("#--- X -------------------------\n",
     "#--- W -------------------------\n"
     "# wzip: WLZ4 and WZIP (plain C); WZIP's levels 7-13 find matches in threads of their own (t#)\n"
     "ifneq ($(wildcard wzip/.),)\n"
     "PLG_FLAGS += -D_WZIP\n"
     "WZIP_SRC := wzip/src\n"
     "$(BUILD)/$(WZIP_SRC)/%.o: $(WZIP_SRC)/%.c\n"
     "\t@mkdir -p $(dir $@)\n"
     "\t$(CC) -O2 $(CFLAGS) -DNDEBUG -DWZIP_MULTITHREAD=1 -c $< -o $@\n"
     "OB += $(call obj,$(addprefix $(WZIP_SRC)/,WLZ4.c WZIP_L.c WZIP_M.c WZIP_wrapper.c Huffman_Compress.c Huffman_Decompress.c))\n"
     "endif\n\n"
     "#--- X -------------------------\n"),
])
print('added wlz4, wlz4fast, wlz4hc and wzip to', T)
