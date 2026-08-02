/*
 * gpu_fft3 — Stockham radix-16 OpenCL FFT for Vivante GC7000UL (no local memory, no barriers).
 *
 * Vivante emulates __local in global DDR, so the classic shared-memory FFT dies (measured
 * 166 FFT/s). This version keeps all butterflies in registers and streams global memory:
 *   pass0 (Ns=1):    load cs16+Hann (16 KB/row)  -> radix-16 in regs -> float2 ping (32 KB)
 *   pass1 (Ns=16):   ping -> radix-16 -> pong
 *   pass2 (Ns=256):  pong -> radix-16 -> |X|^2 -> dB -> u8 fftshifted out (4 KB/row)
 * Stockham auto-sorting: output is naturally ordered, no bit-reversal anywhere.
 * 256 independent work-items per row, no intra-row communication.
 *
 * Build (board rootfs): g++ -O2 -std=c++14 -DCL_TARGET_OPENCL_VERSION=120 gpu_fft3.cpp -o gpu_fft3 -lOpenCL -lm
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
#define R 16
#define NT (N / R)   /* 256 threads per row */

inline float2 cmul(float2 x, float2 y) {
	return (float2)(x.x * y.x - x.y * y.y, x.x * y.y + x.y * y.x);
}

/* in-register 16-pt DFT: two radix-4 stages (DIT on 4x4), v is digit-ordered in/out
 * such that three passes of this with Stockham indexing give a naturally-ordered FFT. */
inline void dft16(float2 *v, __global const float2 *tw) {
	/* stage A: radix-4 over stride 4 */
	for (int m = 0; m < 4; m++) {
		float2 a = v[m], b = v[m + 4], c = v[m + 8], d = v[m + 12];
		float2 t0 = a + c, t1 = a - c, t2 = b + d, t3 = b - d;
		float2 t3j = (float2)(t3.y, -t3.x);
		v[m] = t0 + t2;
		v[m + 4] = t1 + t3j;
		v[m + 8] = t0 - t2;
		v[m + 12] = t1 - t3j;
	}
	/* twiddle W16^(r*m) between stages, then stage B: radix-4 over stride 1 */
	for (int r = 1; r < 4; r++) {
		for (int m = 1; m < 4; m++) {
			v[r * 4 + m] = cmul(v[r * 4 + m], tw[(N / 16) * r * m]);
		}
	}
	for (int r = 0; r < 4; r++) {
		float2 a = v[r * 4], b = v[r * 4 + 1], c = v[r * 4 + 2], d = v[r * 4 + 3];
		float2 t0 = a + c, t1 = a - c, t2 = b + d, t3 = b - d;
		float2 t3j = (float2)(t3.y, -t3.x);
		v[r * 4] = t0 + t2;
		v[r * 4 + 1] = t1 + t3j;
		v[r * 4 + 2] = t0 - t2;
		v[r * 4 + 3] = t1 - t3j;
	}
	/* outputs now at digit-reversed positions: out[k] = v[dr4(k)]; callers index accordingly */
}

/* digit-reverse base-4, 2 digits (0..15) */
__constant int DR16[16] = {0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15};

/* Stockham pass core: thread j of NT handles elements in[j + t*NT], writes
 * out[(j/Ns)*Ns*R + (j%Ns) + dr(t)*Ns] after twiddling by W_N^(t... ) — twiddle BEFORE dft. */

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
	/* Ns=1: no pre-twiddle */
	dft16(v, tw);
	for (int t = 0; t < R; t++) {
		vstore_half2(v[t] * (1.0f/256.0f), j * R + DR16[t], dst);   /* scale into half range */
	}
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
	for (int t = 0; t < R; t++) {
		v[t] = cmul(vload_half2(j + t * NT, src), wt);
		wt = cmul(wt, w1);
	}
	dft16(v, tw);
	int base = ((j >> 4) << 8) + jm;
	for (int t = 0; t < R; t++) {
		vstore_half2(v[t] * (1.0f/16.0f), base + DR16[t] * Ns, dst);
	}
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
	for (int t = 0; t < R; t++) {
		v[t] = cmul(vload_half2(j + t * NT, src), wt);
		wt = cmul(wt, w1);
	}
	dft16(v, tw);
	int base = ((j >> 8) << 12) + jm;
	for (int t = 0; t < R; t++) {
		float2 z = v[t];
		float mag2 = z.x * z.x + z.y * z.y + 1e-12f;
		float db = 3.0102999566f * native_log2(mag2);
		int k = (base + DR16[t] * Ns) ^ 2048;   /* fftshift */
		dst[k] = (uchar)clamp((db - dbMin) * dbScale, 0.0f, 255.0f);
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
	cl_kernel k0 = clCreateKernel(prog, "pass0", &err); CHECK(err);
	cl_kernel k1 = clCreateKernel(prog, "pass1", &err); CHECK(err);
	cl_kernel k2 = clCreateKernel(prog, "pass2", &err); CHECK(err);
	printf("kernels built in %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());

	std::vector<float> win(N);
	std::vector<float> tw(N * 2);
	for (int i = 0; i < N; i++) { win[i] = 0.5f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) * i / (N - 1))); }
	for (int i = 0; i < N; i++) { tw[2 * i] = cosf(-2.0f * static_cast<float>(M_PI) * i / N); tw[2 * i + 1] = sinf(-2.0f * static_cast<float>(M_PI) * i / N); }
	cl_mem bWin = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, N * 4, win.data(), &err); CHECK(err);
	cl_mem bTw = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, N * 8, tw.data(), &err); CHECK(err);
	const float dbMin = -92.0f, dbScale = 255.0f / 190.0f; /* fp16 path: signal scaled by 2^-12 -> dB shifts -72.2 */

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
		cl_mem bPing = clCreateBuffer(ctx, CL_MEM_READ_WRITE, static_cast<size_t>(rows) * N * 4, nullptr, &err); CHECK(err);
		cl_mem bPong = clCreateBuffer(ctx, CL_MEM_READ_WRITE, static_cast<size_t>(rows) * N * 4, nullptr, &err); CHECK(err);
		cl_mem bOut = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, static_cast<size_t>(rows) * N, nullptr, &err); CHECK(err);
		CHECK(clSetKernelArg(k0, 0, sizeof(bIn), &bIn));
		CHECK(clSetKernelArg(k0, 1, sizeof(bPing), &bPing));
		CHECK(clSetKernelArg(k0, 2, sizeof(bWin), &bWin));
		CHECK(clSetKernelArg(k0, 3, sizeof(bTw), &bTw));
		CHECK(clSetKernelArg(k1, 0, sizeof(bPing), &bPing));
		CHECK(clSetKernelArg(k1, 1, sizeof(bPong), &bPong));
		CHECK(clSetKernelArg(k1, 2, sizeof(bTw), &bTw));
		CHECK(clSetKernelArg(k2, 0, sizeof(bPong), &bPong));
		CHECK(clSetKernelArg(k2, 1, sizeof(bOut), &bOut));
		CHECK(clSetKernelArg(k2, 2, sizeof(bTw), &bTw));
		CHECK(clSetKernelArg(k2, 3, sizeof(float), &dbMin));
		CHECK(clSetKernelArg(k2, 4, sizeof(float), &dbScale));
		size_t gsz = static_cast<size_t>(rows) * (N / 16);
		auto run = [&]() {
			clEnqueueNDRangeKernel(q, k0, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr);
			clEnqueueNDRangeKernel(q, k1, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr);
			clEnqueueNDRangeKernel(q, k2, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr);
		};
		run();
		CHECK(clFinish(q));
		std::vector<uint8_t> outRow(N);
		CHECK(clEnqueueReadBuffer(q, bOut, CL_TRUE, 0, N, outRow.data(), 0, nullptr, nullptr));
		int pk = 0;
		for (int i = 0; i < N; i++) { if (outRow[i] > outRow[pk]) { pk = i; } }
		int expect = bin ^ 2048;
		const int reps = rows >= 64 ? 30 : 100;
		auto tt = std::chrono::steady_clock::now();
		for (int i = 0; i < reps; i++) { run(); }
		CHECK(clFinish(q));
		double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tt).count();
		printf("rows=%-4d peak %d (expect %d) %s | %8.0f FFT/s | %6.2f ms/batch | %.1f MSPS coverage\n",
			rows, pk, expect, pk == expect ? "OK" : "WRONG",
			reps * static_cast<double>(rows) / secs, secs / reps * 1e3, reps * static_cast<double>(rows) / secs * N / 1e6);
		clReleaseMemObject(bIn); clReleaseMemObject(bPing); clReleaseMemObject(bPong); clReleaseMemObject(bOut);
	}
	return 0;
}
