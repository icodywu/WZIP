#!/bin/sh
# Builds the figures of doc/*.md: each source in src/ has one TikZ picture per page, and page k becomes the SVG
# named on line k of its list below. Needs pdflatex (with TikZ and the standalone class) and pdftocairo (poppler).
set -e
cd "$(dirname "$0")/src"
build() {   # source, names...
	src=$1; shift
	pdflatex -interaction=nonstopmode -halt-on-error "$src.tex" > /dev/null
	k=1
	for name in "$@"; do
		pdftocairo -svg -f $k -l $k "$src.pdf" "../$name.svg"
		k=$((k + 1))
	done
	rm -f "$src.aux" "$src.log" "$src.pdf"
}
build concepts parsers indexes mwparse
build wlz4 wlz4-block wlz4-offsets wlz4-windows wlz4-example
build wzip wzip-stream wzip-parse wzip-windows wzip-sequence wzip-literals wzip-codes wzip-offsets wzip-m wzip-s
ls ../*.svg
