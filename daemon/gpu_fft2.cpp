/*
 * gpu_fft2 — hand-rolled Vivante GC7000UL OpenCL FFT core for the RFNM spectrum pipeline.
 *
 * One workgroup (256 threads) computes one 4096-pt spectrum entirely in 32 KB local memory:
 *   load:  cs16 IQ -> float2, fused Hann window            (coalesced)
 *   step1: 64 x 64-pt radix-4 DIF FFTs over columns        (digit-reversed out)
 *   step2: inter-step twiddle W4096^(k1*n2), table lookup
 *   step3: 64 x 64-pt radix-4 DIF FFTs over rows
 *   store: |X|^2 -> dB (native_log2) -> u8, digit-reverse + fftshift folded into the index
 * One clEnqueueNDRangeKernel per batch of rows; no global intermediates.
 *
 * Bench mode: correctness (tone->bin) + FFT/s for several batch sizes, vs host expectations.
 * Build (board rootfs): g++ -O2 -std=c++14 -DCL_TARGET_OPENCL_VERSION=120 gpu_fft2.cpp -o gpu_fft2 -lOpenCL -lm
 */

#include <CL/cl.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(x) do { cl_int _e = (x); if (_e != CL_SUCCESS) { fprintf(stderr, "CL error %d at line %d\n", _e, __LINE__); exit(1); } } while (0)

static const char *SRC = R"CLC(
#define N 4096
#define SIDE 64
#define WG 256

/* radix-4 butterfly on 4 complex values (DIF): out0..3 = combined, with W4^r factors */
inline void bfly4(float2 *a, float2 *b, float2 *c, float2 *d) {
	float2 t0 = *a + *c;
	float2 t1 = *a - *c;
	float2 t2 = *b + *d;
	float2 t3 = *b - *d;
	float2 t3j = (float2)(t3.y, -t3.x);   /* -j * t3 */
	*a = t0 + t2;
	*b = t1 + t3j;
	*c = t0 - t2;
	*d = t1 - t3j;
}

inline float2 cmul(float2 x, float2 y) {
	return (float2)(x.x * y.x - x.y * y.y, x.x * y.y + x.y * y.x);
}

/* 64-pt radix-4 DIF FFT over lds[base + idx*stride], 16 threads (u = 0..15).
 * Output is digit-reversed (base-4, 3 digits). w64: W64^k table. */
inline void fft64(__local float2 *lds, int base, int stride, int u, __constant float2 *w64) {
	/* stage 1: quarters of 64, twiddle W64^(r*u1) with u1 = index within 16 */
	{
		float2 a = lds[base + (u) * stride];
		float2 b = lds[base + (u + 16) * stride];
		float2 c = lds[base + (u + 32) * stride];
		float2 d = lds[base + (u + 48) * stride];
		bfly4(&a, &b, &c, &d);
		lds[base + (u) * stride] = a;
		lds[base + (u + 16) * stride] = cmul(b, w64[u]);
		lds[base + (u + 32) * stride] = cmul(c, w64[2 * u]);
		lds[base + (u + 48) * stride] = cmul(d, w64[3 * u]);
	}
	barrier(CLK_LOCAL_MEM_FENCE);
	/* stage 2: within each 16-block, quarters of 16, twiddle W16^(r*u2) = W64^(4*r*u2) */
	{
		int blk = (u >> 2) << 4;      /* 16-block base: 4 blocks per wave handled by 16 threads */
		int u2 = u & 3;
		int i0 = base + (blk + u2) * stride;
		float2 a = lds[i0];
		float2 b = lds[i0 + 4 * stride];
		float2 c = lds[i0 + 8 * stride];
		float2 d = lds[i0 + 12 * stride];
		bfly4(&a, &b, &c, &d);
		lds[i0] = a;
		lds[i0 + 4 * stride] = cmul(b, w64[4 * u2]);
		lds[i0 + 8 * stride] = cmul(c, w64[8 * u2]);
		lds[i0 + 12 * stride] = cmul(d, w64[12 * u2]);
	}
	barrier(CLK_LOCAL_MEM_FENCE);
	/* stage 3: within each 4-block, no twiddles */
	{
		int i0 = base + (u * 4) * stride;
		float2 a = lds[i0];
		float2 b = lds[i0 + stride];
		float2 c = lds[i0 + 2 * stride];
		float2 d = lds[i0 + 3 * stride];
		bfly4(&a, &b, &c, &d);
		lds[i0] = a;
		lds[i0 + stride] = b;
		lds[i0 + 2 * stride] = c;
		lds[i0 + 3 * stride] = d;
	}
	barrier(CLK_LOCAL_MEM_FENCE);
}

__kernel __attribute__((reqd_work_group_size(WG, 1, 1)))
void spectrum4k(__global const short2 *in,      /* [rows][N] cs16 IQ */
                __global uchar *out,            /* [rows][N] u8 dB rows, fftshifted */
                __global const float *win,      /* [N] window */
                __global const float2 *tw,      /* [N] W4096^k */
                __constant float2 *w64,         /* [64] W64^k */
                __constant uchar *dr,           /* [64] digit-reverse base-4 of 0..63 */
                float dbMin, float dbScale)
{
	__local float2 lds[N];
	int row = get_group_id(0);
	int t = get_local_id(0);
	__global const short2 *src = in + row * N;
	__global uchar *dst = out + row * N;

	/* load + convert + window (coalesced: t, t+256, ...) */
	for (int j = 0; j < N / WG; j++) {
		int i = t + j * WG;
		short2 s = src[i];
		lds[i] = (float2)((float)s.x, (float)s.y) * win[i];
	}
	barrier(CLK_LOCAL_MEM_FENCE);

	/* step1: 64 column FFTs (over n1: lds[n1*64 + n2], stride 64), 16 at a time */
	{
		int f = t >> 4;               /* 0..15: FFT slot in this wave */
		int u = t & 15;
		for (int wgrp = 0; wgrp < 4; wgrp++) {
			int n2 = wgrp * 16 + f;
			fft64(lds, n2, SIDE, u, w64);
		}
	}
	/* step2: twiddle: element (p, n2) *= W4096^(dr[p] * n2) */
	for (int j = 0; j < N / WG; j++) {
		int i = t + j * WG;
		int p = i >> 6, n2 = i & 63;
		lds[i] = cmul(lds[i], tw[(int)dr[p] * n2]);
	}
	barrier(CLK_LOCAL_MEM_FENCE);
	/* step3: 64 row FFTs (over n2: lds[p*64 + n2], stride 1) */
	{
		int f = t >> 4;
		int u = t & 15;
		for (int wgrp = 0; wgrp < 4; wgrp++) {
			int p = wgrp * 16 + f;
			fft64(lds, p * SIDE, 1, u, w64);
		}
	}
	/* store: bin k = dr[p] + 64*dr[q]; fftshift = ^2048; mag->dB->u8 */
	for (int j = 0; j < N / WG; j++) {
		int i = t + j * WG;
		int p = i >> 6, q = i & 63;
		float2 z = lds[i];
		float mag2 = z.x * z.x + z.y * z.y + 1e-12f;
		float db = 3.0102999566f * native_log2(mag2);   /* 10*log10 */
		float v = (db - dbMin) * dbScale;
		int k = ((int)dr[p] + 64 * (int)dr[q]) ^ 2048;
		dst[k] = (uchar)clamp(v, 0.0f, 255.0f);
	}
}
)CLC";

int main(int argc, char **argv) {
	const int N = 4096;
	cl_platform_id platform;
	cl_uint np;
	CHECK(clGetPlatformIDs(1, &platform, &np));
	cl_device_id dev;
	CHECK(clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &dev, nullptr));
	cl_int err;
	cl_context ctx = clCreateContext(nullptr, 1, &dev, nullptr, nullptr, &err);
	CHECK(err);
	cl_command_queue q = clCreateCommandQueue(ctx, dev, 0, &err);
	CHECK(err);

	auto t0 = std::chrono::steady_clock::now();
	const char *srcs[] = {SRC};
	cl_program prog = clCreateProgramWithSource(ctx, 1, srcs, nullptr, &err);
	CHECK(err);
	err = clBuildProgram(prog, 1, &dev, "-cl-fast-relaxed-math", nullptr, nullptr);
	if (err != CL_SUCCESS) {
		size_t sz;
		clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &sz);
		std::vector<char> log(sz + 1);
		clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sz, log.data(), nullptr);
		fprintf(stderr, "BUILD FAILED:\n%s\n", log.data());
		return 1;
	}
	cl_kernel k = clCreateKernel(prog, "spectrum4k", &err);
	if (err != CL_SUCCESS) { fprintf(stderr, "clCreateKernel failed: %d (finalizer/resource reject)\n", err); return 1; }
	double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	printf("kernel built in %.0f ms\n", buildMs);

	/* tables */
	std::vector<float> win(N);
	std::vector<float> tw(N * 2), w64(64 * 2);
	std::vector<uint8_t> dr(64);
	for (int i = 0; i < N; i++) { win[i] = 0.5f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) * i / (N - 1))); }
	for (int i = 0; i < N; i++) { tw[2 * i] = cosf(-2.0f * static_cast<float>(M_PI) * i / N); tw[2 * i + 1] = sinf(-2.0f * static_cast<float>(M_PI) * i / N); }
	for (int i = 0; i < 64; i++) { w64[2 * i] = cosf(-2.0f * static_cast<float>(M_PI) * i / 64); w64[2 * i + 1] = sinf(-2.0f * static_cast<float>(M_PI) * i / 64); }
	for (int i = 0; i < 64; i++) { int r = 0, x = i; for (int d = 0; d < 3; d++) { r = (r << 2) | (x & 3); x >>= 2; } dr[i] = static_cast<uint8_t>(r); }

	cl_mem bWin = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, N * 4, win.data(), &err); CHECK(err);
	cl_mem bTw = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, N * 8, tw.data(), &err); CHECK(err);
	cl_mem bW64 = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, 64 * 8, w64.data(), &err); CHECK(err);
	cl_mem bDr = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, 64, dr.data(), &err); CHECK(err);

	const float dbMin = -20.0f, dbMax = 170.0f; /* int16-scale power in dB, generous window */
	const float dbScale = 255.0f / (dbMax - dbMin);

	for (int rows : {1, 16, 64, 256}) {
		std::vector<int16_t> in(static_cast<size_t>(rows) * N * 2);
		const int bin = N / 5;
		for (int r = 0; r < rows; r++) {
			for (int i = 0; i < N; i++) {
				double ph = 2.0 * M_PI * bin * i / N;
				in[(static_cast<size_t>(r) * N + i) * 2] = static_cast<int16_t>(lrint(12000 * cos(ph)));
				in[(static_cast<size_t>(r) * N + i) * 2 + 1] = static_cast<int16_t>(lrint(12000 * sin(ph)));
			}
		}
		cl_mem bIn = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, in.size() * 2, in.data(), &err); CHECK(err);
		cl_mem bOut = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, static_cast<size_t>(rows) * N, nullptr, &err); CHECK(err);
		CHECK(clSetKernelArg(k, 0, sizeof(bIn), &bIn));
		CHECK(clSetKernelArg(k, 1, sizeof(bOut), &bOut));
		CHECK(clSetKernelArg(k, 2, sizeof(bWin), &bWin));
		CHECK(clSetKernelArg(k, 3, sizeof(bTw), &bTw));
		CHECK(clSetKernelArg(k, 4, sizeof(bW64), &bW64));
		CHECK(clSetKernelArg(k, 5, sizeof(bDr), &bDr));
		CHECK(clSetKernelArg(k, 6, sizeof(float), &dbMin));
		CHECK(clSetKernelArg(k, 7, sizeof(float), &dbScale));
		size_t gsz = static_cast<size_t>(rows) * 256, lsz = 256;
		/* warm-up + correctness */
		CHECK(clEnqueueNDRangeKernel(q, k, 1, nullptr, &gsz, &lsz, 0, nullptr, nullptr));
		CHECK(clFinish(q));
		std::vector<uint8_t> outRow(N);
		CHECK(clEnqueueReadBuffer(q, bOut, CL_TRUE, 0, N, outRow.data(), 0, nullptr, nullptr));
		int pk = 0;
		for (int i = 0; i < N; i++) { if (outRow[i] > outRow[pk]) { pk = i; } }
		int expect = bin ^ 2048; /* fftshifted */
		const int reps = rows >= 64 ? 50 : 200;
		auto tt = std::chrono::steady_clock::now();
		for (int i = 0; i < reps; i++) { clEnqueueNDRangeKernel(q, k, 1, nullptr, &gsz, &lsz, 0, nullptr, nullptr); }
		CHECK(clFinish(q));
		double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tt).count();
		printf("rows=%-4d peak %d (expect %d) %s | %8.0f FFT/s | %.2f ms/dispatch | %.1f MSPS coverage\n",
			rows, pk, expect, pk == expect ? "OK" : "WRONG",
			reps * static_cast<double>(rows) / secs, secs / reps * 1e3, reps * static_cast<double>(rows) / secs * N / 1e6);
		clReleaseMemObject(bIn);
		clReleaseMemObject(bOut);
	}
	return 0;
}
