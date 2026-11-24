// Copyright (c) 2026 Peter Fors
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#define FFUTF_IMPLEMENTATION
#include "utf8_utf16le.h"

#ifdef FFUTF_WITH_SIMDUTF
#include <simdutf_c.h>
#endif

#ifdef FFUTF_BENCH_LARGE
#define BENCH_MAX (32u << 20)
#else
#define BENCH_MAX (1u << 20)
#endif

struct workload {
	char *name;
	char *text;
};

struct implementation {
	char *name;
	bool (*validate)(const char *, size_t);
};

struct converter {
	char *name;
	size_t invalid;
	size_t (*convert)(const char *, size_t, uint16_t *);
};

struct error_converter {
	char *name;
	struct ffutf_result (*convert)(const char *, size_t, uint16_t *);
};

struct reverse_converter {
	char *name;
	size_t invalid;
	size_t (*convert)(const uint16_t *, size_t, char *);
};

static struct workload workloads[] = {
	{ "ASCII", "content-length: 16384\r\n" },
	{ "Latin", "caf\xc3\xa9 " },
	{ "CJK", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" },
	{ "emoji", "\xf0\x9f\x98\x80" },
	{ "mixed", "Content: caf\xc3\xa9 \xe6\x97\xa5 \xf0\x9f\x98\x80; status ok\n" }
};

static struct implementation implementations[] = {
#ifdef FFUTF_BENCH_AVX2
	{ "ffutf AVX2", ffutf__validate_avx2 },
#else
	{ "ffutf", ffutf_validate_utf8 },
#endif
#ifdef FFUTF_WITH_SIMDUTF
	{ "simdutf", simdutf_validate_utf8 },
#endif
};

static struct converter converters[] = {
#ifdef FFUTF_BENCH_AVX2
	{ "ffutf AVX2", SIZE_MAX, ffutf__convert_utf8_to_utf16le_avx2 },
#else
	{ "ffutf", SIZE_MAX, ffutf_convert_utf8_to_utf16le },
#endif
#ifdef FFUTF_WITH_SIMDUTF
	{ "simdutf", 0, simdutf_convert_utf8_to_utf16le },
#endif
};

static struct reverse_converter reverse_converters[] = {
#ifdef FFUTF_BENCH_AVX2
	{ "ffutf AVX2", SIZE_MAX, ffutf__convert_utf16le_to_utf8_avx2 },
#else
	{ "ffutf", SIZE_MAX, ffutf_convert_utf16le_to_utf8 },
#endif
#ifdef FFUTF_WITH_SIMDUTF
	{ "simdutf", 0, simdutf_convert_utf16le_to_utf8 },
#endif
};

static struct error_converter error_converters[] = {
#ifdef FFUTF_BENCH_AVX2
	{ "ffutf AVX2", ffutf__convert_utf16_with_errors_avx2 },
#else
	{ "ffutf", ffutf_convert_utf8_to_utf16le_with_errors },
#endif
#ifdef FFUTF_WITH_SIMDUTF
	{ "simdutf", NULL },
#endif
};

__attribute__((aligned(64)))
static char fixture[BENCH_MAX + 64];

__attribute__((aligned(64)))
static uint16_t converted[BENCH_MAX + 1];
__attribute__((aligned(64)))
static uint16_t reference[BENCH_MAX];

__attribute__((aligned(64)))
static uint8_t utf8_output[BENCH_MAX / 2 * 3 + 1];
static uint8_t utf8_reference[BENCH_MAX / 2 * 3];

// [=]===^=[ fill_fixture ]======================================================[=]
static void fill_fixture(char *input, size_t len, char *text) {
	size_t width = strlen(text);
	size_t offset = 0;

	for(; offset + width <= len; offset += width) {
		memcpy(input + offset, text, width);

	}

	size_t prefix = len - offset;

	while(prefix && ((uint8_t)text[prefix] & 0xc0) == 0x80) {
		--prefix;

	}

	memcpy(input + offset, text, prefix);
	memset(input + offset + prefix, 'x', len - offset - prefix);

}

// [=]===^=[ fill_reverse ]======================================================[=]
static void fill_reverse(uint16_t *input, size_t len, struct workload *workload) {
	char *text = !strcmp(workload->name, "mixed") ? "A\xc3\xa9\xe6\x97\xa5\xf0\x9f\x98\x80: status ok\n" : workload->text;
	uint16_t sample[64];
	size_t width = ffutf__convert_utf8_to_utf16le_scalar((uint8_t *)text, strlen(text), sample);
	size_t i = 0;

	for(; i + width <= len; i += width) {
		memcpy(input + i, sample, width * sizeof(*input));

	}

	size_t prefix = len - i;

	if(prefix && sample[prefix - 1] >= 0xd800 && sample[prefix - 1] < 0xdc00) {
		--prefix;

	}

	memcpy(input + i, sample, prefix * sizeof(*input));

	for(i += prefix; i < len; ++i) {
		input[i] = 'x';

	}
}

// [=]===^=[ check ]============================================================[=]
static bool check(char *input, size_t len) {
	bool expected = ffutf__validate_scalar((uint8_t *)input, len);

	for(size_t i = 0; i < sizeof(implementations) / sizeof(implementations[0]); ++i) {
		if(implementations[i].validate(input, len) != expected) {
			fprintf(stderr, "%s differs from scalar validation on %zu input bytes\n", implementations[i].name, len);
			return false;

		}
	}

	return true;

}

// [=]===^=[ check_conversion ]==================================================[=]
static bool check_conversion(char *input, size_t len) {
	size_t units = ffutf__convert_utf8_to_utf16le_scalar((uint8_t *)input, len, reference);
	size_t end = units == SIZE_MAX ? len : units;

	if((units != SIZE_MAX) != ffutf__validate_scalar((uint8_t *)input, len)) {
		fprintf(stderr, "scalar converter and validator disagree on %zu input bytes\n", len);
		return false;

	}

	for(size_t i = 0; i < sizeof(converters) / sizeof(converters[0]); ++i) {
		memset(converted, 0xa5, (end + 1) * sizeof(*converted));
		size_t result = converters[i].convert(input, len, converted);
		size_t expected = units == SIZE_MAX ? converters[i].invalid : units;

		if(result != expected || converted[end] != 0xa5a5 || (units != SIZE_MAX && memcmp(converted, reference, units * sizeof(*converted)))) {
			fprintf(stderr, "%s differs from scalar conversion on %zu input bytes\n", converters[i].name, len);
			return false;

		}
	}

	return true;

}

// [=]===^=[ check_with_errors ]================================================[=]
static bool check_with_errors(char *input, size_t len) {
	size_t units = ffutf__convert_utf8_to_utf16le_scalar((uint8_t *)input, len, reference);
	struct ffutf_result expected = { FFUTF_SUCCESS, units };
	size_t end = units;

	if(units == SIZE_MAX) {
		expected = ffutf__utf8_error((uint8_t *)input, len, len);
		end = len;

	}

	for(size_t i = 0; i < sizeof(error_converters) / sizeof(error_converters[0]); ++i) {
		memset(converted, 0xa5, (end + 1) * sizeof(*converted));
		struct ffutf_result result;
#ifdef FFUTF_WITH_SIMDUTF
		if(i) {
			simdutf_result baseline = simdutf_convert_utf8_to_utf16le_with_errors(input, len, converted);
			result = (struct ffutf_result){ (enum ffutf_error)baseline.error, baseline.count };

		} else
#endif
		{
			result = error_converters[i].convert(input, len, converted);

		}

		if(result.error != expected.error || result.count != expected.count || converted[end] != 0xa5a5 || (!result.error && memcmp(converted, reference, units * sizeof(*converted)))) {
			fprintf(stderr, "%s returned error %u at %zu, expected %u at %zu on %zu input bytes\n", error_converters[i].name, result.error, result.count, expected.error, expected.count, len);
			return false;

		}
	}

	return true;

}

// [=]===^=[ check_reverse ]======================================================[=]
static bool check_reverse(uint16_t *input, size_t len, size_t *written) {
	size_t bytes = ffutf__convert_utf16le_to_utf8_scalar(input, len, utf8_reference);
	size_t end = bytes == SIZE_MAX ? len * 3 : bytes;
	*written = bytes;

	for(size_t i = 0; i < sizeof(reverse_converters) / sizeof(reverse_converters[0]); ++i) {
		memset(utf8_output, 0xa5, end + 1);
		size_t result = reverse_converters[i].convert(input, len, (char *)utf8_output);
		size_t expected = bytes == SIZE_MAX ? reverse_converters[i].invalid : bytes;

		if(result != expected || utf8_output[end] != 0xa5 || (bytes != SIZE_MAX && memcmp(utf8_output, utf8_reference, bytes))) {
			fprintf(stderr, "%s differs from scalar conversion on %zu input units\n", reverse_converters[i].name, len);
			return false;

		}
	}

	return true;

}

// [=]===^=[ main ]==============================================================[=]
int main(int argc, char **argv) {
	bool converting = false;
	bool checking = false;
	bool reverse = false;
	bool with_errors = false;

	if(argc == 2 && !strcmp(argv[1], "--convert")) {
		converting = true;

	} else if(argc == 2 && !strcmp(argv[1], "--check-convert")) {
		converting = true;
		checking = true;

	} else if(argc == 2 && (!strcmp(argv[1], "--with-errors") || !strcmp(argv[1], "--check-with-errors"))) {
		converting = true;
		with_errors = true;
		checking = !strcmp(argv[1], "--check-with-errors");

	} else if(argc == 2 && !strcmp(argv[1], "--reverse")) {
		reverse = true;

	} else if(argc == 2 && !strcmp(argv[1], "--check-reverse")) {
		reverse = true;
		checking = true;

	} else if(argc == 2 && !strcmp(argv[1], "--check")) {
		checking = true;

	} else if(argc != 1) {
		fprintf(stderr, "usage: %s [--check | --convert | --check-convert | --with-errors | --check-with-errors | --reverse | --check-reverse]\n", argv[0]);
		return 1;

	}

	size_t count = reverse ? sizeof(reverse_converters) / sizeof(reverse_converters[0]) : with_errors ? sizeof(error_converters) / sizeof(error_converters[0]) : converting ? sizeof(converters) / sizeof(converters[0]) : sizeof(implementations) / sizeof(implementations[0]);
	size_t families = sizeof(workloads) / sizeof(workloads[0]);
	bool (*verify)(char *, size_t) = with_errors ? check_with_errors : converting ? check_conversion : check;

#ifdef FFUTF_BENCH_AVX2
	if(!__builtin_cpu_supports("avx2") || ((converting || reverse) && !__builtin_cpu_supports("popcnt")) || (reverse && !__builtin_cpu_supports("bmi2"))) {
		fprintf(stderr, "the forced benchmark requires AVX2, POPCNT for conversion, and BMI2 for reverse conversion\n");
		return 1;

	}
#endif

	if(checking && reverse) {
		size_t bytes;
		uint16_t boundaries[] = { 0, 0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xd800, 0xdbff, 0xdc00, 0xdfff, 0xe000, 0xffff };

		if(!check_reverse(converted, 0, &bytes)) {
			return 1;

		}

		for(uint32_t word = 0; word <= UINT16_MAX; ++word) {
			converted[0] = word;

			if(!check_reverse(converted, 1, &bytes)) {
				return 1;

			}
		}

		for(size_t offset = 0; offset < 32; ++offset) {
			uint16_t *input = converted + offset;

			for(size_t text = 0; text < families; ++text) {
				fill_reverse(input, 135, &workloads[text]);

				for(size_t len = 0; len <= 135; ++len) {
					if(!check_reverse(input, len, &bytes)) {
						return 1;

					}
				}

				for(size_t pos = 28; pos <= 35; ++pos) {
					uint16_t saved = input[pos];

					for(size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
						input[pos] = boundaries[i];

						if(!check_reverse(input, 135, &bytes)) {
							return 1;

						}
					}

					input[pos] = saved;

				}
			}
		}

		puts("UTF-16LE to UTF-8 correctness checks passed.");
		return 0;

	}

	if(checking) {
		uint8_t pair[2];

		if(with_errors) {
			char broken[130];
			memset(broken, 'x', sizeof(broken));
			broken[63] = (char)0xe0;
			broken[64] = (char)0xa0;
			broken[65] = (char)0xff;
			broken[129] = (char)0xc2;
			struct ffutf_result result = error_converters[0].convert(broken, sizeof(broken), converted);

			if(result.error != FFUTF_TOO_SHORT || result.count != 63 || !verify(broken, sizeof(broken))) {
				fprintf(stderr, "conversion did not report the first malformed sequence\n");
				return 1;

			}
		}

		if(!verify(NULL, 0)) {
			return 1;

		}

		for(uint32_t a = 0; a < 256; ++a) {
			pair[0] = a;

			if(!verify((char *)pair, 1)) {
				return 1;

			}

			for(uint32_t b = 0; b < 256; ++b) {
				pair[1] = b;

				if(!verify((char *)pair, 2)) {
					return 1;

				}
			}
		}

		for(size_t text = 0; text < families; ++text) {
			for(size_t misalignment = 0; misalignment < 64; ++misalignment) {
				char *input = fixture + misalignment;
				fill_fixture(input, 263, workloads[text].text);

				for(size_t len = 0; len <= 263; ++len) {
					if(!verify(input, len)) {
						return 1;

					}
				}

				for(size_t position = 60; position <= 67; ++position) {
					uint8_t saved = input[position];

					for(uint32_t byte = 0; byte < 256; ++byte) {
						input[position] = byte;

						if(!verify(input, 263)) {
							return 1;

						}
					}

					input[position] = saved;

				}
			}
		}

		puts(with_errors ? "UTF-8 to UTF-16LE error reporting checks passed." : converting ? "UTF-8 to UTF-16LE correctness checks passed." : "UTF-8 correctness checks passed.");
		return 0;

	}

	size_t utf8_lengths[] = { 16, 31, 32, 33, 63, 64, 65, 128, 255, 256, 257, 4096, 65536, BENCH_MAX };
	size_t utf16_lengths[] = { 16, 32, 62, 64, 66, 128, 256, 4096, 65536, 262144, 1u << 20, 4u << 20, 16u << 20, 32u << 20 };
	size_t *lengths = reverse ? utf16_lengths : utf8_lengths;
	size_t rows = reverse ? sizeof(utf16_lengths) / sizeof(utf16_lengths[0]) : sizeof(utf8_lengths) / sizeof(utf8_lengths[0]);
	printf("%-8s %9s", "input", "bytes");

	for(size_t method = 0; method < count; ++method) {
		printf(" %16s", reverse ? reverse_converters[method].name : with_errors ? error_converters[method].name : converting ? converters[method].name : implementations[method].name);

	}

	puts(reverse ? "   input GB/s" : "   ns/call");

	for(size_t text = 0; text < families; ++text) {
		for(size_t row = 0; row < rows; ++row) {
			size_t len = lengths[row];
			size_t repeats = (reverse ? 128u << 20 : 8u << 20) / len;

			if(len > BENCH_MAX) {
				continue;

			}

			if(converting && repeats < 128) {
				repeats = 128;

			}

			double rates[count][3];
			size_t units = reverse ? len / 2 : 0;
			size_t bytes = 0;

			if(reverse) {
				fill_reverse(converted, units, &workloads[text]);

				if(!check_reverse(converted, units, &bytes)) {
					return 1;

				}

			} else {
				fill_fixture(fixture, len, workloads[text].text);

				if(!verify(fixture, len)) {
					return 1;

				}

				units = converting ? ffutf__convert_utf8_to_utf16le_scalar((uint8_t *)fixture, len, reference) : 0;

			}

			if(units == SIZE_MAX || bytes == SIZE_MAX) {
				fprintf(stderr, "invalid conversion fixture\n");
				return 1;

			}

			for(size_t trial = 0; trial < 3; ++trial) {
				for(size_t order = 0; order < count; ++order) {
					size_t method = (order + trial) % count;
					struct timespec start, end;
					size_t accepted = 0;

					if(reverse) {
						memset(utf8_output, 0xa5, bytes + 1);

					} else if(converting) {
						memset(converted, 0xa5, (units + 1) * sizeof(*converted));

					}

					if(clock_gettime(CLOCK_MONOTONIC, &start)) {
						perror("clock_gettime");
						return 1;

					}

					if(reverse) {
						size_t (*convert)(const uint16_t *, size_t, char *) = reverse_converters[method].convert;

						for(size_t call = 0; call < repeats; ++call) {
							__asm__ volatile("" : "+m"(*(uint16_t (*)[units])converted));
							size_t result = convert(converted, units, (char *)utf8_output);
							__asm__ volatile("" : : "m"(*(uint8_t (*)[bytes])utf8_output));
							accepted += result == bytes;

						}

					} else if(with_errors) {
#ifdef FFUTF_WITH_SIMDUTF
						if(method) {
							simdutf_result (*convert)(const char *, size_t, uint16_t *) = simdutf_convert_utf8_to_utf16le_with_errors;

							for(size_t call = 0; call < repeats; ++call) {
								__asm__ volatile("" : "+m"(*(char (*)[len])fixture));
								simdutf_result result = convert(fixture, len, converted);
								__asm__ volatile("" : : "m"(*(uint16_t (*)[units])converted));
								accepted += result.error == SIMDUTF_ERROR_SUCCESS && result.count == units;

							}
						} else
#endif
						{
							struct ffutf_result (*convert)(const char *, size_t, uint16_t *) = error_converters[method].convert;

							for(size_t call = 0; call < repeats; ++call) {
								__asm__ volatile("" : "+m"(*(char (*)[len])fixture));
								struct ffutf_result result = convert(fixture, len, converted);
								__asm__ volatile("" : : "m"(*(uint16_t (*)[units])converted));
								accepted += result.error == FFUTF_SUCCESS && result.count == units;

							}
						}

					} else if(converting) {
						size_t (*convert)(const char *, size_t, uint16_t *) = converters[method].convert;

						for(size_t call = 0; call < repeats; ++call) {
							__asm__ volatile("" : "+m"(*(char (*)[len])fixture));
							size_t result = convert(fixture, len, converted);
							__asm__ volatile("" : : "m"(*(uint16_t (*)[units])converted));
							accepted += result == units;

						}

					} else {
						bool (*validate)(const char *, size_t) = implementations[method].validate;

						for(size_t call = 0; call < repeats; ++call) {
							__asm__ volatile("" : "+m"(*(char (*)[len])fixture));
							accepted += validate(fixture, len);

						}
					}

					if(clock_gettime(CLOCK_MONOTONIC, &end)) {
						perror("clock_gettime");
						return 1;

					}

					if(reverse && (utf8_output[bytes] != 0xa5 || memcmp(utf8_output, utf8_reference, bytes))) {
						fprintf(stderr, "%s produced incorrect UTF-8 output\n", reverse_converters[method].name);
						return 1;

					}

					if(accepted != repeats || (converting && (converted[units] != 0xa5a5 || memcmp(converted, reference, units * sizeof(*converted))))) {
						fprintf(stderr, "%s failed a valid fixture\n", reverse ? reverse_converters[method].name : with_errors ? error_converters[method].name : converting ? converters[method].name : implementations[method].name);
						return 1;

					}

					int64_t elapsed = (int64_t)(end.tv_sec - start.tv_sec) * INT64_C(1000000000) + end.tv_nsec - start.tv_nsec;
					rates[method][trial] = (double)elapsed / repeats;

				}
			}

			printf("%-8s %9zu", workloads[text].name, len);

			for(size_t method = 0; method < count; ++method) {
				double a = rates[method][0];
				double b = rates[method][1];
				double c = rates[method][2];
				double low = a < b ? a : b;
				double high = a > b ? a : b;
				double median = c < low ? low : c > high ? high : c;
				printf(" %16.3f", reverse ? len / median : median);

			}

			putchar('\n');

		}
	}

	return 0;

}
