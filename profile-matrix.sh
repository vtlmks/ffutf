#!/usr/bin/env bash
# Copyright (c) 2026 Peter Fors
# SPDX-License-Identifier: MIT
set -euo pipefail

if [[ $# != 2 ]]; then
	printf 'usage: %s BINARY_DIRECTORY LOGICAL_CPU\n' "$0" >&2
	exit 1
fi

cd -- "$1"
cpu=$2
group='{cycles:u,instructions:u,branch-misses:u,cpu/event=0x24,umask=0x02/u}'
cases=('avx512 0 4 128 0 0' 'avx512 0 4 129 0 0' 'avx512 2 1 256 0 0' 'avx512 2 1 256 0 256' 'avx2 2 4 30 0 0' 'avx512 1 3 129 0 0')

for trial in 0 1 2; do
	for row in "${cases[@]}"; do
		read -r isa op workload size input output <<< "$row"
		for rank in 0 1; do
			method=$(( (rank + trial) % 2 ))
			printf 'case %s %s %s\n' "$trial" "$row" "$method"
			taskset -c "$cpu" chrt -o 0 perf stat -x, -e "$group" \
				"./matrix-$isa" --profile "$op" "$workload" "$size" "$input" "$output" "$method"
		done
	done
done
