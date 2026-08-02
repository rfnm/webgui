/*
 * fft_bench — A53 NEON FFT budget measurement for the RFNM spectrum daemon.
 *
 * Per-spectrum pipeline, exactly what the daemon will run per row:
 *   1. CS16 interleaved -> float complex, fused with a Q15-scaled Hann window (NEON)
 *   2. pffft complex forward FFT (NEON)
 *   3. |X|^2 -> dB (fast log2 approx) -> u8 quantize (NEON-ish scalar mix)
 * Measures aggregate spectra/s for 1..4 worker threads at N = 2048/4096/8192.
 *
 * Build (in the board rootfs): g++ -O3 -o fft_bench fft_bench.cpp third_party/pffft.c -lm -lpthread
 */

#include <arm_neon.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "third_party/pffft.h"

static void make_window(float *w, int n) {
	for (int i = 0; i < n; i++) {
		w[i] = 0.5f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) * i / (n - 1)));
	}
}

/* CS16 interleaved IQ + float window -> float complex interleaved (NEON, 8 samples/iter) */
static void convert_window(const int16_t *in, const float *win, float *out, int n) {
	for (int i = 0; i < n; i += 4) {
		int16x4x2_t iq = vld2_s16(in + 2 * i);
		float32x4_t re = vcvtq_f32_s32(vmovl_s16(iq.val[0]));
		float32x4_t im = vcvtq_f32_s32(vmovl_s16(iq.val[1]));
		float32x4_t w = vld1q_f32(win + i);
		float32x4x2_t o;
		o.val[0] = vmulq_f32(re, w);
		o.val[1] = vmulq_f32(im, w);
		vst2q_f32(out + 2 * i, o);
	}
}

/* |X|^2 -> dB-ish u8: db = 10*log10(p) via log2 exponent trick, then scale/clamp */
static void mag_db_u8(const float *fft, uint8_t *out, int n, float db_min, float db_max) {
	const float scale = 255.0f / (db_max - db_min);
	for (int i = 0; i < n; i += 4) {
		float32x4_t re = vld2q_f32(fft + 2 * i).val[0];
		float32x4_t im = vld2q_f32(fft + 2 * i).val[1];
		float32x4_t p = vmlaq_f32(vmulq_f32(re, re), im, im);
		/* fast log2 via float bit pattern: log2(p) ~= (bits/2^23) - 127 + mantissa poly (skip poly: +-0.09 dB banding acceptable? use 1-term correction) */
		int32x4_t bits = vreinterpretq_s32_f32(p);
		float32x4_t log2p = vmulq_n_f32(vcvtq_f32_s32(bits), 1.0f / (1 << 23));
		log2p = vsubq_f32(log2p, vdupq_n_f32(127.0f));
		float32x4_t db = vmulq_n_f32(log2p, 3.0102999566f); /* 10*log10(2) */
		float32x4_t v = vmulq_n_f32(vsubq_f32(db, vdupq_n_f32(db_min)), scale);
		int32x4_t iv = vcvtq_s32_f32(vmaxq_f32(vminq_f32(v, vdupq_n_f32(255.0f)), vdupq_n_f32(0.0f)));
		out[i] = static_cast<uint8_t>(vgetq_lane_s32(iv, 0));
		out[i + 1] = static_cast<uint8_t>(vgetq_lane_s32(iv, 1));
		out[i + 2] = static_cast<uint8_t>(vgetq_lane_s32(iv, 2));
		out[i + 3] = static_cast<uint8_t>(vgetq_lane_s32(iv, 3));
	}
}

struct WorkerResult { uint64_t spectra; };

static void worker(int n, double seconds, WorkerResult *res, std::atomic<bool> *stop) {
	PFFFT_Setup *setup = pffft_new_setup(n, PFFFT_COMPLEX);
	float *win = static_cast<float *>(pffft_aligned_malloc(n * 4));
	float *fin = static_cast<float *>(pffft_aligned_malloc(n * 8));
	float *fout = static_cast<float *>(pffft_aligned_malloc(n * 8));
	float *work = static_cast<float *>(pffft_aligned_malloc(n * 8));
	int16_t *cs16 = static_cast<int16_t *>(pffft_aligned_malloc(n * 4));
	uint8_t *row = static_cast<uint8_t *>(pffft_aligned_malloc(n));
	make_window(win, n);
	for (int i = 0; i < 2 * n; i++) { cs16[i] = static_cast<int16_t>((i * 2654435761u) >> 20); }
	uint64_t cnt = 0;
	while (!stop->load(std::memory_order_relaxed)) {
		convert_window(cs16, win, fin, n);
		pffft_transform_ordered(setup, fin, fout, work, PFFFT_FORWARD);
		mag_db_u8(fout, row, n, -110.0f, -10.0f);
		cnt++;
	}
	res->spectra = cnt;
	pffft_aligned_free(win); pffft_aligned_free(fin); pffft_aligned_free(fout);
	pffft_aligned_free(work); pffft_aligned_free(cs16); pffft_aligned_free(row);
	pffft_destroy_setup(setup);
}

int main(int argc, char **argv) {
	double secs = argc > 1 ? atof(argv[1]) : 3.0;
	printf("%-6s %-8s %12s %14s %12s\n", "N", "threads", "spectra/s", "us/spectrum", "MSPS-cover");
	for (int n : {2048, 4096, 8192}) {
		for (int t = 1; t <= 4; t++) {
			std::atomic<bool> stop{false};
			std::vector<WorkerResult> res(t);
			std::vector<std::thread> th;
			for (int i = 0; i < t; i++) { th.emplace_back(worker, n, secs, &res[i], &stop); }
			std::this_thread::sleep_for(std::chrono::duration<double>(secs));
			stop = true;
			for (auto &x : th) { x.join(); }
			uint64_t total = 0;
			for (auto &r : res) { total += r.spectra; }
			double sps = total / secs;
			printf("%-6d %-8d %12.0f %14.1f %11.1fM\n", n, t, sps, t * 1e6 / (sps / 1.0), sps * n / 1e6);
		}
	}
	return 0;
}
