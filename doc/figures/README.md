# Figures of the format specifications

The SVG files here are generated; edit the TikZ sources in `src/` and run `sh build.sh` (needs `pdflatex` with
TikZ and the `standalone` class, and `pdftocairo` from poppler). Each source holds one picture per page, and the
script names page k after the k-th name it lists.

| Source | Figures |
|---|---|
| `src/concepts.tex` | `parsers`, `indexes`, `mwparse`: unchanged from the IEEE Trans. IT paper (`papers/WLZ.pdf`) |
| `src/wlz4.tex` | `wlz4-block`, `wlz4-offsets`, `wlz4-windows`, `wlz4-example` (`doc/WLZ4_format.md`) |
| `src/wzip.tex` | `wzip-stream`, `wzip-parse`, `wzip-windows`, `wzip-sequence`, `wzip-literals`, `wzip-codes`, `wzip-offsets`, `wzip-m`, `wzip-s`, `wzip-size` (`doc/WZIP_format.md`) |

`wlz4-position.svg`, a measurement rather than a format figure, comes from `bench/plot_position.py` (see
`bench/README.md`).

`src/style.tex` holds the shared colors: yellow for headers and sizes, blue for tokens and joint symbols, gray for
literals, orange for offsets, green for extensions and extra bits, purple for code tables. Every figure has a white
background, so that it stays legible in a dark page theme.
