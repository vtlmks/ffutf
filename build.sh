#!/bin/sh
# Copyright (c) 2026 Peter Fors
# SPDX-License-Identifier: MIT
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O2 -Wall -Wextra}
OUTPUT=${OUTPUT:-bench}

if [ -n "${SIMDUTF_DIR:-}" ]; then
	SIMDUTF_DIR=$(CDPATH= cd -- "$SIMDUTF_DIR" && pwd)
	simdutf_lib_dir=$(CDPATH= cd -- "${SIMDUTF_LIB_DIR:-$SIMDUTF_DIR}" && pwd)
	set -- -DFFUTF_WITH_SIMDUTF -I"$SIMDUTF_DIR" -L"$simdutf_lib_dir" -Wl,-rpath,"$simdutf_lib_dir" -lsimdutf
else
	set --
fi

"$CC" -std=c99 $CFLAGS "$source_dir/bench.c" "$@" -o "$OUTPUT"
