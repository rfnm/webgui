/*
 * dsp_bench — HiFi4 bring-up tool for the RFNM boards (rebased Auterion DSP fw @ 0xA4000000).
 *
 * Subcommands:
 *   poison-fill    fill the OLD DSP window [0x92400000,0x94400000) with a PRNG pattern, print hash
 *   poison-check   re-hash the old window, PASS iff identical to poison-fill's output
 *   validate       tone-FFT correctness at 1024/2048/4096(/8192) via cplx16x16 + float
 *   bench          BENCHMARK_FLAG stats for single-shot FFTs + OFDM batch 16x16 throughput
 *
 * Transport is RPMsg only (MU fast path deliberately disabled for bring-up).
 * Build: aarch64-linux-gnu-g++ -O2 -static -I<rootfs>/usr/include dsp_bench.cpp -o dsp_bench
 */

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dspoffload/DspOffloader.hpp"

#define OLD_WIN_BASE 0x92400000UL
#define OLD_WIN_SIZE 0x02000000UL
#define POISON_SEED  0x52464E4DUL /* "RFNM" */

/* xorshift32 PRNG — deterministic pattern, same sequence for fill and check */
static inline uint32_t xs32(uint32_t &s) {
	s ^= s << 13; s ^= s >> 17; s ^= s << 5;
	return s;
}

static void *map_phys(uint64_t base, size_t size) {
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open /dev/mem"); exit(1); }
	void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
	close(fd);
	if (p == MAP_FAILED) { perror("mmap"); exit(1); }
	return p;
}

static int poison(bool fill) {
	volatile uint32_t *w = static_cast<volatile uint32_t *>(map_phys(OLD_WIN_BASE, OLD_WIN_SIZE));
	uint32_t s = POISON_SEED;
	uint64_t mism = 0, first_bad = 0;
	uint64_t fnv = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < OLD_WIN_SIZE / 4; i++) {
		uint32_t v = xs32(s);
		if (fill) {
			w[i] = v;
		} else {
			uint32_t got = w[i];
			if (got != v && !mism++) { first_bad = OLD_WIN_BASE + i * 4; }
			fnv = (fnv ^ got) * 0x100000001b3ULL;
		}
	}
	if (fill) {
		printf("poison-fill: [%#lx,%#lx) filled, seed %#lx\n", OLD_WIN_BASE, OLD_WIN_BASE + OLD_WIN_SIZE, POISON_SEED);
		return 0;
	}
	if (mism) {
		printf("poison-check: FAIL — %" PRIu64 " mismatching words, first at %#" PRIx64 " (fnv %#" PRIx64 ")\n", mism, first_bad, fnv);
		return 1;
	}
	printf("poison-check: PASS — old window byte-identical (fnv %#" PRIx64 ")\n", fnv);
	return 0;
}

struct Ctx {
	DspOffloader dsp;
	volatile uint8_t *shm;
	bool init() {
		dsp.use_mu = false; /* RPMsg only for bring-up */
		if (!dsp.init()) { fprintf(stderr, "DspOffloader init failed (rpmsg up? fw running?)\n"); return false; }
		dsp.set_use_mu(false);
		shm = static_cast<volatile uint8_t *>(dsp.get_shared_mem());
		return shm != nullptr;
	}
};

/* fill a complex tone at the given bin into shared mem as int16 IQ pairs */
static void write_tone_cs16(volatile uint8_t *shm, uint32_t off, uint32_t n, uint32_t bin, int16_t amp) {
	volatile int16_t *p = reinterpret_cast<volatile int16_t *>(shm + off);
	for (uint32_t i = 0; i < n; i++) {
		double ph = 2.0 * M_PI * bin * i / n;
		p[2 * i] = static_cast<int16_t>(lrint(amp * cos(ph)));
		p[2 * i + 1] = static_cast<int16_t>(lrint(amp * sin(ph)));
	}
}

static uint32_t peak_bin_cs16(volatile uint8_t *shm, uint32_t off, uint32_t n) {
	volatile int16_t *p = reinterpret_cast<volatile int16_t *>(shm + off);
	uint64_t best = 0; uint32_t bi = 0;
	for (uint32_t i = 0; i < n; i++) {
		int64_t re = p[2 * i], im = p[2 * i + 1];
		uint64_t m = static_cast<uint64_t>(re * re + im * im);
		if (m > best) { best = m; bi = i; }
	}
	return bi;
}

static int validate(Ctx &c) {
	int fails = 0;
	const uint32_t OFF_IN = 0x100000, OFF_OUT = 0x200000;
	for (uint32_t n : {1024u, 2048u, 4096u, 8192u}) {
		uint32_t bin = n / 5;
		write_tone_cs16(c.shm, OFF_IN, n, bin, 12000);
		memset(const_cast<uint8_t *>(c.shm) + OFF_OUT, 0, n * 4);
		auto r = c.dsp.execute_with_data_type(DSP_CMD_FFT_CPLX16X16, n, OFF_IN, 0, OFF_OUT, DSP_DATA_COMPLEX_INT16);
		if (!r.success) {
			printf("validate fft_cplx16x16 N=%-5u: EXEC FAIL (status %u)\n", n, r.status);
			fails++;
			continue;
		}
		uint32_t pk = peak_bin_cs16(c.shm, OFF_OUT, n);
		bool ok = pk == bin;
		printf("validate fft_cplx16x16 N=%-5u: peak %u (expect %u) %s  dsp=%uus rt=%uus\n", n, pk, bin, ok ? "OK" : "WRONG", r.dsp_time_us, r.roundtrip_us);
		if (!ok) { fails++; }
	}
	return fails;
}

static void bench_single(Ctx &c) {
	const uint32_t OFF_IN = 0x100000, OFF_OUT = 0x200000;
	struct { dsp_cmd_id cmd; const char *name; uint32_t dt; uint32_t bytes; } ops[] = {
		{DSP_CMD_FFT_CPLX16X16, "fft_cplx16x16", DSP_DATA_COMPLEX_INT16, 4},
		{DSP_CMD_FFT_CPLX32X32, "fft_cplx32x32", DSP_DATA_COMPLEX_INT32, 8},
		{DSP_CMD_FFT_CPLXF, "fft_cplxf", DSP_DATA_COMPLEX_FLOAT, 8},
	};
	printf("\n-- single-shot benchmarks (DSP_CMD_BENCHMARK_FLAG, 100 iters) --\n");
	for (auto &op : ops) {
		for (uint32_t n : {1024u, 2048u, 4096u, 8192u}) {
			write_tone_cs16(c.shm, OFF_IN, n, n / 5, 12000);
			auto r = c.dsp.execute_with_data_type(static_cast<dsp_cmd_id>(op.cmd | DSP_CMD_BENCHMARK_FLAG), n, OFF_IN, 0, OFF_OUT, op.dt);
			if (!r.success) {
				printf("%-15s N=%-5u: FAIL (status %u)\n", op.name, n, r.status);
				continue;
			}
			double fps = r.avg_time_us ? 1e6 / r.avg_time_us : 0;
			printf("%-15s N=%-5u: avg %5u us (min %5u max %5u)  -> %7.0f FFT/s, verr=%u\n", op.name, n, r.avg_time_us, r.min_time_us, r.max_time_us, fps, r.validation_errors);
		}
	}
}

static void bench_ofdm(Ctx &c) {
	printf("\n-- OFDM batch FFT 16x16 (iDMA ping-pong) --\n");
	const uint32_t CFG_OFF = 0x080000, OFF_IN = 0x100000, OFF_OUT = 0x600000;
	for (uint32_t n : {2048u, 4096u}) {
		for (uint32_t nsym : {8u, 32u, 64u}) {
			auto *cfg = reinterpret_cast<volatile ofdm_batch_config *>(const_cast<uint8_t *>(c.shm) + CFG_OFF);
			memset(const_cast<uint8_t *>(c.shm) + CFG_OFF, 0, sizeof(*cfg));
			cfg->fft_size = n;
			cfg->cp_len = 0;
			cfg->num_symbols = nsym;
			cfg->data_type = 1; /* complex int16 */
			cfg->input_offset = OFF_IN;
			cfg->output_offset = OFF_OUT;
			cfg->flags = OFDM_FLAG_USE_IDMA;
			for (uint32_t s = 0; s < nsym; s++) {
				write_tone_cs16(c.shm, OFF_IN + s * n * 4, n, n / 5, 12000);
			}
			auto rc = c.dsp.execute(DSP_CMD_OFDM_BATCH_CONFIG, 0, CFG_OFF, 0, 0);
			if (!rc.success) {
				printf("ofdm config N=%u nsym=%u: FAIL (status %u)\n", n, nsym, rc.status);
				continue;
			}
			/* warm-up + timed reps */
			c.dsp.execute(DSP_CMD_OFDM_BATCH_FFT_16X16, 0, 0, 0, 0);
			const int reps = 20;
			auto t0 = std::chrono::steady_clock::now();
			int ok = 0;
			for (int i = 0; i < reps; i++) {
				auto r = c.dsp.execute(DSP_CMD_OFDM_BATCH_FFT_16X16, 0, 0, 0, 0);
				ok += r.success;
			}
			double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			double ffts = static_cast<double>(reps) * nsym / secs;
			uint32_t pk = peak_bin_cs16(c.shm, OFF_OUT, n);
			printf("ofdm_batch16 N=%-5u nsym=%-3u: %6.0f FFT/s (%d/%d ok, %.1f ms/batch) peak %u %s\n",
				n, nsym, ffts, ok, reps, secs / reps * 1e3, pk, pk == n / 5 ? "OK" : "WRONG");
		}
	}
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s poison-fill|poison-check|validate|bench\n", argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "poison-fill")) { return poison(true); }
	if (!strcmp(argv[1], "poison-check")) { return poison(false); }
	Ctx c;
	if (!c.init()) { return 1; }
	if (!strcmp(argv[1], "validate")) {
		int f = validate(c);
		printf(f ? "VALIDATE: %d FAILURES\n" : "VALIDATE: ALL OK\n", f);
		return f ? 1 : 0;
	}
	if (!strcmp(argv[1], "bench")) {
		int f = validate(c);
		bench_single(c);
		bench_ofdm(c);
		return f ? 1 : 0;
	}
	fprintf(stderr, "unknown subcommand %s\n", argv[1]);
	return 2;
}
