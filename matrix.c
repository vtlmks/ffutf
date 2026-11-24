// Copyright (c) 2026 Peter Fors
// SPDX-License-Identifier: MIT

#define _GNU_SOURCE
#define FFUTF_WITH_SIMDUTF
#define main ffutf_bench_main
#include "bench.c"
#undef main

#include <errno.h>
#include <linux/perf_event.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MATRIX_MAX (64u << 20)
#define MATRIX_TRIALS 5

enum operation {
	VALIDATE,
	FORWARD,
	REVERSE,
	WITH_ERRORS
};

struct placement {
	char *name;
	size_t input;
	size_t output;
};

struct sample {
	enum operation operation;
	size_t workload;
	size_t len;
	size_t input_offset;
	size_t output_shift;
	char *placement;
	int64_t error_at;
	uint8_t *input;
	uint8_t *output;
	size_t output_bytes;
	size_t count;
	enum ffutf_error error;
};

struct cycle_reading {
	uint64_t cycles;
	uint64_t enabled;
	uint64_t running;
};

union matrix_arena {
	uint8_t bytes[3 * (size_t)MATRIX_MAX + 8192];
	uint16_t words[(3 * (size_t)MATRIX_MAX + 8192) / 2];
};

static char *operation_names[] = { "validate_utf8", "utf8_to_utf16le", "utf16le_to_utf8", "utf8_to_utf16le_with_errors" };

__attribute__((aligned(4096)))
static union matrix_arena arena;

__attribute__((aligned(64)))
static uint16_t wanted[MATRIX_MAX];

#ifndef FFUTF_BENCH_AVX2
// [=]===^=[ validate_vbmi ]=====================================================[=]
__attribute__((noinline))
static bool validate_vbmi(const char *input, size_t len) {
	return ffutf__validate_avx512((uint8_t *)input, len);

}

// [=]===^=[ forward_vbmi ]======================================================[=]
__attribute__((noinline))
static size_t forward_vbmi(const char *input, size_t len, uint16_t *output) {
	return ffutf__convert_utf8_to_utf16le_avx512((uint8_t *)input, len, output);

}

// [=]===^=[ reverse_vbmi ]======================================================[=]
__attribute__((noinline))
static size_t reverse_vbmi(const uint16_t *input, size_t len, char *output) {
	return ffutf__convert_utf16le_to_utf8_avx512((uint16_t *)input, len, (uint8_t *)output);

}

// [=]===^=[ errors_vbmi ]=======================================================[=]
__attribute__((noinline))
static struct ffutf_result errors_vbmi(const char *input, size_t len, uint16_t *output) {
	return ffutf__convert_utf16_with_errors_avx512((uint8_t *)input, len, output);

}
#endif

// [=]===^=[ now ]==============================================================[=]
static uint64_t now(void) {
	struct timespec value;

	if(clock_gettime(CLOCK_MONOTONIC_RAW, &value)) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);

	}

	return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;

}

// [=]===^=[ read_cycles ]=======================================================[=]
static struct cycle_reading read_cycles(int fd) {
	struct cycle_reading value;

	if(read(fd, &value, sizeof(value)) != sizeof(value)) {
		perror("read perf_event");
		exit(EXIT_FAILURE);

	}

	return value;

}

// [=]===^=[ prepare ]===========================================================[=]
static void prepare(struct sample *sample) {
	size_t input = 0x240 + sample->input_offset;
	size_t output = MATRIX_MAX + 0x280 + sample->output_shift;
	sample->input = sample->operation == REVERSE ? (uint8_t *)(arena.words + input / 2) : arena.bytes + input;
	sample->output = sample->operation == REVERSE || sample->operation == VALIDATE ? arena.bytes + output : (uint8_t *)(arena.words + output / 2);
	struct workload *workload = workloads + sample->workload;

	if(sample->operation == REVERSE) {
		fill_reverse((uint16_t *)sample->input, sample->len / 2, workload);
		sample->count = ffutf__convert_utf16le_to_utf8_scalar((uint16_t *)sample->input, sample->len / 2, (uint8_t *)wanted);
		sample->output_bytes = sample->count;

	} else {
		fill_fixture((char *)sample->input, sample->len, workload->text);

		if(sample->error_at >= 0) {
			sample->input[sample->error_at] = 0xff;
			struct ffutf_result error = ffutf__utf8_error(sample->input, sample->len, sample->len);
			sample->error = error.error;
			sample->count = error.count;
			sample->output_bytes = 2 * sample->len;

		} else if(sample->operation != VALIDATE) {
			sample->count = ffutf__convert_utf8_to_utf16le_scalar(sample->input, sample->len, wanted);
			sample->output_bytes = 2 * sample->count;

		}
	}

	if((sample->operation != VALIDATE && sample->count == SIZE_MAX) || (sample->error_at < 0 && sample->operation != REVERSE && !ffutf__validate_scalar(sample->input, sample->len))) {
		fputs("could not prepare the benchmark fixture\n", stderr);
		exit(EXIT_FAILURE);

	}
}

#define TIME_CALL(type, call, correct)                                            \
	__asm__ volatile("" : "+r"(fn));                                              \
	for(size_t i = 0; i < repeats; ++i) {                                          \
		__asm__ volatile("" : "+m"(*(uint8_t (*)[s->len])s->input));                 \
		type result = (call);                                                     \
		__asm__ volatile("" : : "m"(*(uint8_t (*)[visible_bytes])s->output));       \
		accepted += (correct);                                                    \
                                                                                  \
	}

// [=]===^=[ measure ]==========================================================[=]
static void measure(struct sample *s, uint32_t method, size_t trial, size_t repeats, int fd) {
	size_t accepted = 0;
	size_t visible_bytes = s->output_bytes ? s->output_bytes : 1;
	size_t units = s->len / 2;
	char *input = (char *)s->input;
	char *output = (char *)s->output;
	uint64_t start, elapsed;
	struct cycle_reading before, after;

	memset(output, 0xa5, s->output_bytes + 1);
	before = read_cycles(fd);
	start = now();

	switch(s->operation) {
		case VALIDATE: {
#ifdef FFUTF_BENCH_AVX2
			bool (*fn)(const char *, size_t) = implementations[method].validate;
#else
			bool (*fn)(const char *, size_t) = method ? simdutf_validate_utf8 : validate_vbmi;
#endif
			TIME_CALL(bool, fn(input, s->len), result);
		} break;

		case FORWARD: {
#ifdef FFUTF_BENCH_AVX2
			size_t (*fn)(const char *, size_t, uint16_t *) = converters[method].convert;
#else
			size_t (*fn)(const char *, size_t, uint16_t *) = method ? simdutf_convert_utf8_to_utf16le : forward_vbmi;
#endif
			TIME_CALL(size_t, fn(input, s->len, (uint16_t *)output), result == s->count);
		} break;

		case REVERSE: {
#ifdef FFUTF_BENCH_AVX2
			size_t (*fn)(const uint16_t *, size_t, char *) = reverse_converters[method].convert;
#else
			size_t (*fn)(const uint16_t *, size_t, char *) = method ? simdutf_convert_utf16le_to_utf8 : reverse_vbmi;
#endif
			TIME_CALL(size_t, fn((uint16_t *)input, units, output), result == s->count);
		} break;

		case WITH_ERRORS: {
			if(method) {
				simdutf_result (*fn)(const char *, size_t, uint16_t *) = simdutf_convert_utf8_to_utf16le_with_errors;
				TIME_CALL(simdutf_result, fn(input, s->len, (uint16_t *)output), (uint32_t)result.error == s->error && result.count == s->count);

			} else {
#ifdef FFUTF_BENCH_AVX2
				struct ffutf_result (*fn)(const char *, size_t, uint16_t *) = error_converters[0].convert;
#else
				struct ffutf_result (*fn)(const char *, size_t, uint16_t *) = errors_vbmi;
#endif
				TIME_CALL(struct ffutf_result, fn(input, s->len, (uint16_t *)output), result.error == s->error && result.count == s->count);

			}
		} break;
	}

	elapsed = now() - start;
	after = read_cycles(fd);

	if(accepted != repeats || s->output[s->output_bytes] != 0xa5 || (s->error_at < 0 && s->output_bytes && memcmp(output, wanted, s->output_bytes))) {
		fprintf(stderr, "incorrect result for %s, %s, %zu bytes, %s, method %u\n", operation_names[s->operation], workloads[s->workload].name, s->len, s->placement, method);
		exit(EXIT_FAILURE);

	}

	if(after.running - before.running != after.enabled - before.enabled) {
		fputs("cycle counter was multiplexed\n", stderr);
		exit(EXIT_FAILURE);

	}

	printf("%s,%s,%zu,%s,%zu,%zu,%zu,%jd,%s,%zu,%zu,%ju,%ju\n", operation_names[s->operation], workloads[s->workload].name, s->len, s->placement, s->input_offset, s->output_shift, s->output_bytes, (intmax_t)s->error_at, method ? "simdutf" : "ffutf", trial, repeats, (uintmax_t)elapsed, (uintmax_t)(after.cycles - before.cycles));

}

#undef TIME_CALL

// [=]===^=[ run_case ]==========================================================[=]
static void run_case(struct sample *s, size_t row, int fd) {
	size_t bytes = s->error_at >= 0 ? (size_t)s->error_at + 1 : s->len;
	size_t repeats = (4u << 20) / bytes;

	if(repeats < 2) {
		repeats = 2;

	} else if(repeats > 131072) {
		repeats = 131072;

	}

	prepare(s);

	for(size_t trial = 0; trial < MATRIX_TRIALS; ++trial) {
		for(uint32_t order = 0; order < 2; ++order) {
			measure(s, (uint32_t)((order + trial + row) & 1), trial, repeats, fd);

		}
	}
}

// [=]===^=[ main ]==============================================================[=]
int main(int argc, char **argv) {
#ifdef FFUTF_BENCH_AVX2
	if(!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("bmi2") || !__builtin_cpu_supports("popcnt")) {
#else
	if(!__builtin_cpu_supports("avx512vbmi") || !__builtin_cpu_supports("avx512vbmi2") || !__builtin_cpu_supports("popcnt")) {
#endif
		fputs("CPU lacks the required instruction set\n", stderr);
		return EXIT_FAILURE;

	}

	struct perf_event_attr attr = { .type = PERF_TYPE_HARDWARE, .size = sizeof(attr), .config = PERF_COUNT_HW_CPU_CYCLES, .read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING, .exclude_kernel = 1, .exclude_hv = 1 };
	int fd = syscall(SYS_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC);

	if(fd < 0) {
		perror("perf_event_open");
		return EXIT_FAILURE;

	}

	puts("operation,workload,input_bytes,placement,input_offset,output_shift,output_bytes,error_at,implementation,trial,repeats,elapsed_ns,cycles");

	if(argc == 8 && !strcmp(argv[1], "--profile")) {
		size_t op = strtoul(argv[2], NULL, 10);
		size_t text = strtoul(argv[3], NULL, 10);
		size_t size = strtoul(argv[4], NULL, 10);
		size_t offset = strtoul(argv[5], NULL, 10);
		size_t shift = strtoul(argv[6], NULL, 10);
		size_t method = strtoul(argv[7], NULL, 10);

		if(op > WITH_ERRORS || text >= sizeof(workloads) / sizeof(*workloads) || !size || size > MATRIX_MAX || offset >= 4096 || shift >= 4096 || size + offset > (size_t)MATRIX_MAX + 64 + shift || (op == REVERSE && (size % 2 || offset % 2)) || ((op == FORWARD || op == WITH_ERRORS) && shift % 2) || method > 1) {
			fputs("invalid profile case\n", stderr);
			close(fd);
			return EXIT_FAILURE;

		}

		struct sample sample = { .operation = op, .workload = text, .len = size, .input_offset = offset, .output_shift = shift, .placement = "profile", .error_at = -1 };
		size_t repeats = (512u << 20) / size;

		if(repeats > 20000000) {
			repeats = 20000000;

		}

		prepare(&sample);
		measure(&sample, method, 0, repeats, fd);
		close(fd);
		return EXIT_SUCCESS;

	}

	if(argc != 1) {
		fputs("usage: matrix [--profile OP TEXT INPUT_BYTES INPUT_OFFSET OUTPUT_SHIFT METHOD]\n", stderr);
		close(fd);
		return EXIT_FAILURE;

	}

	size_t lengths[] = { 16, 31, 32, 33, 63, 64, 65, 127, 128, 129, 192, 255, 256, 257, 512, 4096, 32768, 262144, 1048576, 4194304, 16777216, 33554432, MATRIX_MAX };
	size_t row = 0;

	for(enum operation op = VALIDATE; op <= WITH_ERRORS; ++op) {
		struct placement placements[] = { { "aligned", 0, 0 }, { op == VALIDATE ? "offset32" : "output_plus_128", op == VALIDATE ? 32 : 0, op == VALIDATE ? 0 : 128 }, { "split", op == REVERSE ? 62 : 63, op == VALIDATE ? 0 : op == REVERSE ? 63 : 62 } };

		for(size_t text = 0; text < sizeof(workloads) / sizeof(*workloads); ++text) {
			for(size_t j = 0; j < sizeof(lengths) / sizeof(*lengths); ++j) {
				size_t length = lengths[j];

				if(op == REVERSE && length % 2) {
					length = (j == 1 || j == 4 || j == 7 || j == 11) ? length - 1 : length + 1;

				}

				for(size_t p = 0; p < sizeof(placements) / sizeof(*placements); ++p) {
					struct sample sample = { .operation = op, .workload = text, .len = length, .input_offset = placements[p].input, .output_shift = placements[p].output, .placement = placements[p].name, .error_at = -1 };
					run_case(&sample, row++, fd);

				}
			}
		}
	}

	size_t error_sizes[] = { 4096, 1048576 };

	for(size_t j = 0; j < sizeof(error_sizes) / sizeof(*error_sizes); ++j) {
		size_t len = error_sizes[j];
		size_t positions[] = { 0, len / 2, len - 1 };

		for(size_t p = 0; p < sizeof(positions) / sizeof(*positions); ++p) {
			struct sample sample = { .operation = WITH_ERRORS, .workload = 0, .len = len, .placement = "aligned", .error_at = positions[p] };
			run_case(&sample, row++, fd);

		}
	}

	close(fd);
	return EXIT_SUCCESS;

}
