// Shared spectrum FFT engine (GC7000UL OpenCL, Stockham radix-16, fp16 intermediates:
// cs16+Hann load -> 3 register-resident passes -> |X|^2 -> dB -> u8 fftshifted). Extracted
// verbatim from spectrumd so ismmond reuses the IDENTICAL GPU FFT; spectrumd #includes this too
// (its only change). Interface: GpuFFT::init(dbMin,dbMax); fill inMap[slot] with `rows` windows of
// N_FFT CS16 samples; run(slot, rows_out, rows) -> rows*N_FFT u8-dB values (fftshifted).
#pragma once

#include <CL/cl.h>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <vector>

#ifndef N_FFT
#define N_FFT 4096
#endif
#ifndef BATCH
#define BATCH 64                 /* rows per GPU dispatch */
#endif

static inline void fft_log(const char *fmt, ...) {
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	fprintf(stderr, "[fft_engine] %s\n", b);
}

static const char *CL_SRC = R"CLC(
#define N 4096
#define R 16
#define NT (N / R)
inline float2 cmul(float2 x, float2 y) { return (float2)(x.x*y.x - x.y*y.y, x.x*y.y + x.y*y.x); }
inline void dft16(float2 *v, __global const float2 *tw) {
	for (int m = 0; m < 4; m++) {
		float2 a = v[m], b = v[m+4], c = v[m+8], d = v[m+12];
		float2 t0 = a+c, t1 = a-c, t2 = b+d, t3 = b-d;
		float2 t3j = (float2)(t3.y, -t3.x);
		v[m] = t0+t2; v[m+4] = t1+t3j; v[m+8] = t0-t2; v[m+12] = t1-t3j;
	}
	for (int r = 1; r < 4; r++) for (int m = 1; m < 4; m++) v[r*4+m] = cmul(v[r*4+m], tw[(N/16)*r*m]);
	for (int r = 0; r < 4; r++) {
		float2 a = v[r*4], b = v[r*4+1], c = v[r*4+2], d = v[r*4+3];
		float2 t0 = a+c, t1 = a-c, t2 = b+d, t3 = b-d;
		float2 t3j = (float2)(t3.y, -t3.x);
		v[r*4] = t0+t2; v[r*4+1] = t1+t3j; v[r*4+2] = t0-t2; v[r*4+3] = t1-t3j;
	}
}
__constant int DR16[16] = {0,4,8,12,1,5,9,13,2,6,10,14,3,7,11,15};

__kernel void pass0(__global const short2 *in, __global half *out, __global const float *win, __global const float2 *tw) {
	int gid = get_global_id(0);
	int row = gid >> 8, j = gid & (NT - 1);
	__global const short2 *src = in + row * N;
	__global half *dst = out + row * N * 2;
	float2 v[R];
	for (int t = 0; t < R; t++) {
		int i = j + t * NT;
		short2 s = src[i];
		v[t] = (float2)((float)s.x, (float)s.y) * win[i];
	}
	dft16(v, tw);
	for (int t = 0; t < R; t++) vstore_half2(v[t] * (1.0f/256.0f), j * R + DR16[t], dst);
}
__kernel void pass1(__global const half *in, __global half *out, __global const float2 *tw) {
	const int Ns = 16;
	int gid = get_global_id(0);
	int row = gid >> 8, j = gid & (NT - 1);
	__global const half *src = in + row * N * 2;
	__global half *dst = out + row * N * 2;
	float2 v[R];
	int jm = j & (Ns - 1);
	float2 w1 = tw[(N / (Ns * R)) * jm];
	float2 wt = (float2)(1.0f, 0.0f);
	#pragma unroll
	for (int t = 0; t < R; t++) { v[t] = cmul(vload_half2(j + t * NT, src), wt); wt = cmul(wt, w1); }
	dft16(v, tw);
	int base = ((j >> 4) << 8) + jm;
	for (int t = 0; t < R; t++) vstore_half2(v[t] * (1.0f/16.0f), base + DR16[t] * Ns, dst);
}
__kernel void pass2(__global const half *in, __global uchar *out, __global const float2 *tw, float dbMin, float dbScale) {
	const int Ns = 256;
	int gid = get_global_id(0);
	int row = gid >> 8, j = gid & (NT - 1);
	__global const half *src = in + row * N * 2;
	__global uchar *dst = out + row * N;
	float2 v[R];
	int jm = j & (Ns - 1);
	float2 w1 = tw[(N / (Ns * R)) * jm];
	float2 wt = (float2)(1.0f, 0.0f);
	#pragma unroll
	for (int t = 0; t < R; t++) { v[t] = cmul(vload_half2(j + t * NT, src), wt); wt = cmul(wt, w1); }
	dft16(v, tw);
	int base = ((j >> 8) << 12) + jm;
	for (int t = 0; t < R; t++) {
		float2 z = v[t];
		float mag2 = z.x * z.x + z.y * z.y + 1e-12f;
		float db = 3.0102999566f * native_log2(mag2);
		int k = (base + DR16[t] * Ns) ^ 2048;
		dst[k] = (uchar)clamp((db - dbMin) * dbScale, 0.0f, 255.0f);
	}
}
)CLC";

struct GpuFFT {
	cl_context ctx = nullptr;
	cl_command_queue q = nullptr;
	cl_kernel k0 = nullptr, k1 = nullptr, k2 = nullptr;
	cl_mem bIn[2] = {nullptr, nullptr}, bPing = nullptr, bPong = nullptr, bOut = nullptr, bWin = nullptr, bTw = nullptr;
	int16_t *inMap[2] = {nullptr, nullptr};
	float avg_batch_ms = 0;

	bool init(float dbMin, float dbMax) {
		cl_platform_id platform;
		cl_uint n;
		if (clGetPlatformIDs(1, &platform, &n) != CL_SUCCESS || !n) { return false; }
		cl_device_id dev;
		if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &dev, nullptr) != CL_SUCCESS) { return false; }
		cl_int err;
		ctx = clCreateContext(nullptr, 1, &dev, nullptr, nullptr, &err);
		q = clCreateCommandQueue(ctx, dev, 0, &err);
		const char *srcs[] = {CL_SRC};
		cl_program prog = clCreateProgramWithSource(ctx, 1, srcs, nullptr, &err);
		if (clBuildProgram(prog, 1, &dev, "-cl-fast-relaxed-math", nullptr, nullptr) != CL_SUCCESS) {
			size_t sz;
			clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &sz);
			std::vector<char> log(sz + 1);
			clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sz, log.data(), nullptr);
			fft_log("CL build failed: %s", log.data());
			return false;
		}
		k0 = clCreateKernel(prog, "pass0", &err);
		k1 = clCreateKernel(prog, "pass1", &err);
		k2 = clCreateKernel(prog, "pass2", &err);
		if (!k0 || !k1 || !k2) { return false; }

		std::vector<float> win(N_FFT), tw(N_FFT * 2);
		for (int i = 0; i < N_FFT; i++) { win[i] = 0.5f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) * i / (N_FFT - 1))); }
		for (int i = 0; i < N_FFT; i++) {
			tw[2 * i] = cosf(-2.0f * static_cast<float>(M_PI) * i / N_FFT);
			tw[2 * i + 1] = sinf(-2.0f * static_cast<float>(M_PI) * i / N_FFT);
		}
		bWin = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, N_FFT * 4, win.data(), &err);
		bTw = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, N_FFT * 8, tw.data(), &err);
		for (int i = 0; i < 2; i++) {
			bIn[i] = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR, static_cast<size_t>(BATCH) * N_FFT * 4, nullptr, &err);
			inMap[i] = static_cast<int16_t *>(clEnqueueMapBuffer(q, bIn[i], CL_TRUE, CL_MAP_WRITE, 0, static_cast<size_t>(BATCH) * N_FFT * 4, 0, nullptr, nullptr, &err));
			if (!inMap[i]) { fft_log("map failed"); return false; }
		}
		bPing = clCreateBuffer(ctx, CL_MEM_READ_WRITE, static_cast<size_t>(BATCH) * N_FFT * 4, nullptr, &err);
		bPong = clCreateBuffer(ctx, CL_MEM_READ_WRITE, static_cast<size_t>(BATCH) * N_FFT * 4, nullptr, &err);
		bOut = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, static_cast<size_t>(BATCH) * N_FFT, nullptr, &err);
		clSetKernelArg(k0, 1, sizeof(bPing), &bPing);
		clSetKernelArg(k0, 2, sizeof(bWin), &bWin);
		clSetKernelArg(k0, 3, sizeof(bTw), &bTw);
		clSetKernelArg(k1, 0, sizeof(bPing), &bPing);
		clSetKernelArg(k1, 1, sizeof(bPong), &bPong);
		clSetKernelArg(k1, 2, sizeof(bTw), &bTw);
		clSetKernelArg(k2, 0, sizeof(bPong), &bPong);
		clSetKernelArg(k2, 1, sizeof(bOut), &bOut);
		clSetKernelArg(k2, 2, sizeof(bTw), &bTw);
		float scale = 255.0f / (dbMax - dbMin);
		clSetKernelArg(k2, 3, sizeof(float), &dbMin);
		clSetKernelArg(k2, 4, sizeof(float), &scale);
		return true;
	}

	void set_scale(float dbMin, float dbMax) {
		float scale = 255.0f / (dbMax - dbMin);
		clSetKernelArg(k2, 3, sizeof(float), &dbMin);
		clSetKernelArg(k2, 4, sizeof(float), &scale);
	}

	/* Exercise the whole CL pipeline with one throwaway row. The galcore stack power-gates
	 * the GPU after long idle and the FIRST enqueue after a multi-minute gap can park
	 * forever in gckOS_WaitSignal (the resume-after-park RT freeze); a periodic keepalive
	 * during idle keeps the context/hardware ungated. Uses a dedicated input buffer and a
	 * host scratch so it never touches the producer-mapped inputs or the row ring; only
	 * ever called from the gpu worker thread, so it cannot race run(). */
	cl_mem bKeep = nullptr;
	bool keepalive() {
		static uint8_t scratch[N_FFT];
		cl_int err;
		if (!bKeep) {
			bKeep = clCreateBuffer(ctx, CL_MEM_READ_ONLY, static_cast<size_t>(N_FFT) * 4, nullptr, &err);
			if (!bKeep) { return false; }
		}
		clSetKernelArg(k0, 0, sizeof(cl_mem), &bKeep);
		size_t gsz = N_FFT / 16;
		clEnqueueNDRangeKernel(q, k0, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr);
		clEnqueueNDRangeKernel(q, k1, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr);
		clEnqueueNDRangeKernel(q, k2, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr);
		return clEnqueueReadBuffer(q, bOut, CL_TRUE, 0, N_FFT, scratch, 0, nullptr, nullptr) == CL_SUCCESS;
	}

	/* slot: which mapped input buffer the producer filled. Blocking. */
	bool run(int slot, uint8_t *rows_out, int rows) {
		auto t0 = std::chrono::steady_clock::now();
		clEnqueueUnmapMemObject(q, bIn[slot], inMap[slot], 0, nullptr, nullptr);
		clSetKernelArg(k0, 0, sizeof(cl_mem), &bIn[slot]);
		size_t gsz = static_cast<size_t>(rows) * (N_FFT / 16);
		clEnqueueNDRangeKernel(q, k0, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr);
		clEnqueueNDRangeKernel(q, k1, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr);
		clEnqueueNDRangeKernel(q, k2, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr);
		if (clEnqueueReadBuffer(q, bOut, CL_TRUE, 0, static_cast<size_t>(rows) * N_FFT, rows_out, 0, nullptr, nullptr) != CL_SUCCESS) { return false; }
		cl_int merr;
		inMap[slot] = static_cast<int16_t *>(clEnqueueMapBuffer(q, bIn[slot], CL_TRUE, CL_MAP_WRITE, 0, static_cast<size_t>(rows) * N_FFT * 4, 0, nullptr, nullptr, &merr));
		float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
		avg_batch_ms = avg_batch_ms * 0.9f + ms * 0.1f;
		return true;
	}
};
