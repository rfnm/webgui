/*
 * gpu_fft_bench — VkFFT-on-Vivante feasibility + throughput for the RFNM spectrum daemon.
 *
 * Measures batched 4096-pt complex forward FFTs on the GC7000UL via VkFFT's OpenCL backend:
 * plan build time, correctness (tone -> peak bin), and FFT/s vs batch size.
 *
 * Build (board rootfs):
 *   g++ -O2 -std=c++14 -DVKFFT_BACKEND=3 -DCL_TARGET_OPENCL_VERSION=120 \
 *       -I/rfnm/spectrum/src/third_party/VkFFT/vkFFT gpu_fft_bench.cpp -o gpu_fft_bench -lOpenCL -lm
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "vkFFT.h"

#define CHECK(x) do { cl_int _e = (x); if (_e != CL_SUCCESS) { fprintf(stderr, "CL error %d at %s:%d\n", _e, __FILE__, __LINE__); exit(1); } } while (0)

int main(int argc, char **argv) {
	const uint64_t N = argc > 1 ? strtoull(argv[1], nullptr, 0) : 4096;
	cl_platform_id platform;
	cl_uint np;
	CHECK(clGetPlatformIDs(1, &platform, &np));
	cl_device_id device;
	CHECK(clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, nullptr));
	cl_int err;
	cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
	CHECK(err);
	cl_command_queue q = clCreateCommandQueue(ctx, device, 0, &err);
	CHECK(err);

	for (uint64_t batch : {1ull, 16ull, 64ull, 256ull}) {
		uint64_t bufSize = N * batch * 8; /* complex float */
		cl_mem buf = clCreateBuffer(ctx, CL_MEM_READ_WRITE, bufSize, nullptr, &err);
		CHECK(err);

		/* tone at bin N/5 in every batch row */
		std::vector<float> host(N * batch * 2);
		for (uint64_t b = 0; b < batch; b++) {
			for (uint64_t i = 0; i < N; i++) {
				double ph = 2.0 * M_PI * (N / 5) * i / N;
				host[(b * N + i) * 2] = static_cast<float>(0.5 * cos(ph));
				host[(b * N + i) * 2 + 1] = static_cast<float>(0.5 * sin(ph));
			}
		}
		CHECK(clEnqueueWriteBuffer(q, buf, CL_TRUE, 0, bufSize, host.data(), 0, nullptr, nullptr));

		VkFFTConfiguration conf = {};
		conf.FFTdim = 1;
		conf.size[0] = N;
		conf.numberBatches = batch;
		conf.device = &device;
		conf.context = &ctx;
		if (getenv("VKFFT_AIM_THREADS")) { conf.aimThreads = strtoul(getenv("VKFFT_AIM_THREADS"), nullptr, 0); }
		if (getenv("VKFFT_MAX_THREADS")) { conf.maxThreadsNum = strtoul(getenv("VKFFT_MAX_THREADS"), nullptr, 0); }
		if (getenv("VKFFT_KEEP_SHADER")) { conf.keepShaderCode = 1; }
		VkFFTApplication app = {};
		auto t0 = std::chrono::steady_clock::now();
		VkFFTResult r = initializeVkFFT(&app, conf);
		double planMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (r != VKFFT_SUCCESS) {
			printf("N=%llu batch=%llu: initializeVkFFT FAILED (%d)\n", (unsigned long long)N, (unsigned long long)batch, r);
			clReleaseMemObject(buf);
			continue;
		}

		VkFFTLaunchParams lp = {};
		lp.buffer = &buf;
		lp.commandQueue = &q;
		/* warm-up + correctness */
		r = VkFFTAppend(&app, -1, &lp);
		clFinish(q);
		if (r != VKFFT_SUCCESS) {
			printf("N=%llu batch=%llu: VkFFTAppend FAILED (%d)\n", (unsigned long long)N, (unsigned long long)batch, r);
			deleteVkFFT(&app);
			clReleaseMemObject(buf);
			continue;
		}
		CHECK(clEnqueueReadBuffer(q, buf, CL_TRUE, 0, N * 8, host.data(), 0, nullptr, nullptr));
		uint64_t pk = 0; double best = -1;
		for (uint64_t i = 0; i < N; i++) {
			double m = static_cast<double>(host[2 * i]) * host[2 * i] + static_cast<double>(host[2 * i + 1]) * host[2 * i + 1];
			if (m > best) { best = m; pk = i; }
		}

		/* timed reps (re-upload not needed; in-place transform of transformed data is fine for timing) */
		const int reps = batch >= 64 ? 20 : 100;
		t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < reps; i++) { VkFFTAppend(&app, -1, &lp); }
		clFinish(q);
		double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		double ffts = static_cast<double>(reps) * batch / secs;
		printf("N=%-5llu batch=%-4llu plan %6.0f ms | peak %llu (expect %llu) %s | %8.0f FFT/s (%.2f ms/dispatch)\n",
			(unsigned long long)N, (unsigned long long)batch, planMs,
			(unsigned long long)pk, (unsigned long long)(N / 5), pk == N / 5 ? "OK" : "WRONG",
			ffts, secs / reps * 1e3);
		deleteVkFFT(&app);
		clReleaseMemObject(buf);
	}
	clReleaseCommandQueue(q);
	clReleaseContext(ctx);
	return 0;
}
