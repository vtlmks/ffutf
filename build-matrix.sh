#!/bin/sh
# Copyright (c) 2026 Peter Fors
# SPDX-License-Identifier: MIT
set -eu

if [ "$#" -ne 2 ]; then
	printf 'usage: %s SIMDUTF_AMALGAMATION OUTPUT_DIRECTORY\n' "$0" >&2
	exit 1
fi

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
simdutf_dir=$(CDPATH= cd -- "$1" && pwd)
mkdir -p -- "$2"
output_dir=$(CDPATH= cd -- "$2" && pwd)
cc=${CC:-clang}
cxx=${CXX:-g++}

cd -- "$output_dir"
for isa in avx2 avx512; do
	mkdir -p "$isa"
	if [ "$isa" = avx2 ]; then
		disabled_backend=-DSIMDUTF_IMPLEMENTATION_ICELAKE=0
		force_avx2=-DFFUTF_BENCH_AVX2
	else
		disabled_backend=-DSIMDUTF_IMPLEMENTATION_HASWELL=0
		force_avx2=
	fi
	"$cxx" -std=c++17 -O3 -DNDEBUG -fPIC -shared -fno-semantic-interposition \
		"$disabled_backend" -DSIMDUTF_IMPLEMENTATION_WESTMERE=0 -DSIMDUTF_IMPLEMENTATION_FALLBACK=0 \
		-DSIMDUTF_USE_STATIC_INITIALIZATION=1 "$simdutf_dir/simdutf.cpp" -o "$isa/libsimdutf.so"
	"$cc" -std=c99 -O2 -Wall -Wextra -Werror $force_avx2 -I"$simdutf_dir" "$source_dir/matrix.c" \
		-L"$isa" "-Wl,-rpath,\$ORIGIN/$isa" -lsimdutf -o "matrix-$isa"
	CC="$cc" SIMDUTF_DIR="$simdutf_dir" SIMDUTF_LIB_DIR="$output_dir/$isa" CFLAGS="-O2 -Wall -Wextra -Werror $force_avx2" \
		OUTPUT="$output_dir/check-$isa" "$source_dir/build.sh"
done
