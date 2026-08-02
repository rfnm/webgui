/*
 * spectrumd — RFNM on-board spectrum daemon.
 *
 *   librfnm TRANSPORT_LOCAL (CS16 @ up to 122.88 MSPS)
 *     -> GC7000UL OpenCL pipeline (Stockham radix-16, fp16 intermediates:
 *        cs16+Hann load -> 3 register-resident passes -> |X|^2->dB->u8 fftshifted)
 *     -> binary WebSocket (RFSP v1, PROTOCOL.md) on 127.0.0.1:7681 behind nginx :80.
 *
 * Threads:
 *   rx      — continuous rx_stream::read into window batches (freshest-wins, keeps the
 *             kernel's 1 s data_ep recency gate fed); or synthetic source (--synthetic)
 *   gpu     — batch upload -> 3 kernels -> u8 readback -> row ring
 *   ws      — epoll server: HTTP upgrade, SPECTRA batches, STATUS 1 Hz, CONFIG/ACK
 *
 * Build (board rootfs):
 *   g++ -O3 -std=c++17 -DCL_TARGET_OPENCL_VERSION=120 spectrumd.cpp -o spectrumd \
 *       -I/usr/local/include -L... librfnm.a -lOpenCL -lusb-1.0 -lpthread -lm
 */

#include <CL/cl.h>
#include <zlib.h>
#include <arm_neon.h>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <thread>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>

#include <cerrno>
#include <sys/uio.h>

#include "rfnm_spectrum_protocol.h"
#include "sigmf.h"

extern "C" {
#include "third_party/pffft.h"
}

#define RFSP_HDRSZ_SPECTRA 64

#ifndef NO_LIBRFNM
#include <librfnm/device.h>
#include <librfnm/rx_stream.h>
#endif

#include "fft_engine.h"      /* GpuFFT + CL kernels + N_FFT/BATCH, shared with ismmond */
#include "pacer.h"           /* per-client WAN flow control (ACK-clocked BDP window) */
#define ROW_RING 512             /* output rows buffered for WS */
#define RX_PARK_GRACE_MS 4000u   /* park the RX stream after this long with no WS client */
#define WS_ROWS_PER_MSG 16

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

static void logts(const char *fmt, ...) {
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	fprintf(stderr, "[spectrumd] %s\n", b);
}

/* CS16 interleaved + float window -> float complex (NEON) */
static void cw_convert(const int16_t *in, const float *win, float *out, int n) {
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

/* |X|^2 -> dB -> u8 with fftshift; dbMin here is in TRUE dB (GPU path's fp16 scale offset
 * of -72.25 dB is added by the caller so both engines produce identical u8 values) */
static void cw_mag_u8(const float *fft, uint8_t *out, int n, float dbMin, float dbScale) {
	for (int i = 0; i < n; i++) {
		float re = fft[2 * i], im = fft[2 * i + 1];
		float mag2 = re * re + im * im + 1e-12f;
		int e;
		float m = frexpf(mag2, &e);
		float db = 3.0102999566f * (static_cast<float>(e) + 2.0f * (m - 0.75f));  /* fast log2 approx */
		float v = (db - dbMin) * dbScale;
		int k = (i ^ 2048);
		out[k] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
	}
}

static bool g_dc_notch = true;

/* Replace the LO-leakage spike (center +-3 bins after fftshift) with the median of the
 * surrounding bins — display-level DC removal, same practice as every SDR GUI. */
static void dc_notch_rows(uint8_t *rows, int nrows) {
	const int c = N_FFT / 2;
	for (int r = 0; r < nrows; r++) {
		uint8_t *row = rows + static_cast<size_t>(r) * N_FFT;
		int l = row[c - 2], rt = row[c + 2];
		int ref = l > rt ? l : rt;
		if (static_cast<int>(row[c]) - ref < 6 && static_cast<int>(row[c - 1]) - ref < 6 && static_cast<int>(row[c + 1]) - ref < 6) { continue; }
		row[c - 1] = static_cast<uint8_t>((3 * l + rt) / 4);
		row[c] = static_cast<uint8_t>((l + rt) / 2);
		row[c + 1] = static_cast<uint8_t>((l + 3 * rt) / 4);
	}
}

static void pin_self(int cpu) {
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

/* ------------------------------------------------------------------ config */

struct Cfg {
	double freq = 100e6;         /* RX LO */
	int gain = 0;                /* reference gain: manual value, AGC seed AND the absolute level anchor
	                              * the sweep AGC normalizes the composed row back to */
	bool agc = true;             /* leveling on by default, HOST-side in both modes (RT: stepper on the rx
	                              * thread's own stream; sweep: per-hop table). The kernel rfnm_agc loop is
	                              * never engaged: it reads the raw iqflood ADC ring, which is NOT in a
	                              * layout it understands under local CS16 / DCS-rate sessions (reproduced:
	                              * peak pinned 2048, gain railed to -24; the
	                              * module is format-gated now, mirroring the rfnm_qec lesson).
	                              * --no-agc or an explicit TUNE gain turns leveling off. */
	double rate = 100e6;
	uint16_t port = 7681;
	bool synthetic = false;
	int synth_rows = 3000;       /* synthetic rows/s */
	float db_min = -92.0f, db_max = 98.0f;   /* GPU compute-domain dB window (fp16 path) */
	uint32_t max_rows = 3840;    /* default client cap, CONFIG overrides */
	std::string path = "";      /* RF path: "", "embed", "a".."h", "loopback" */
	bool dc_notch = true;        /* replace the center bins with the local median before shipping */
	int cpu_fft = 2;             /* additional A53 pffft workers */
};

/* live-retune mailbox (ws thread -> rx thread) */
struct TuneBox {
	std::atomic<uint64_t> freq_mhz1000{0};   /* freq in kHz; 0 = nothing pending */
	std::atomic<uint32_t> rate_req{0};       /* requested sample rate Hz; 0 = none (zoom span control) */
	std::atomic<int32_t> gain{INT32_MIN};    /* INT32_MIN = nothing pending; an explicit gain disables AGC */
	std::atomic<int32_t> agc_req{-1};        /* -1 = none pending; 0/1 = AGC off/on (TUNE flags bit2) */
	std::atomic<int32_t> path_req{-1};       /* -1 = none pending; else (channel << 8) | path code */
};

/* live AGC state for the 1 Hz STATUS readback (rx thread writes, ws thread reads).
 * gain = the actual RX gain (the RT stepper's live value; the reference gain in sweep). */
static std::atomic<int32_t> g_agc_gain{0};
static std::atomic<uint32_t> g_agc_on{0};
/* 2026-07-21 reform: full-IQ detection is the default again - the DC guard (rfnm_lime)
 * makes an ADC rail impossible by construction and the null keeps the lanes centered,
 * so the Q lane is trustworthy. --i-only-detector restores the old workaround. */
static int g_iq_det = 1;
/* client-door dc-null: opt-in until the 2x2 knob-to-lane plant study at the local DCS
 * clock plans lands (2026-07-21 receipts: at the 100M plan neither DCOFF knob moved the
 * delivered-Q lane; the restore path caught it, but an unresponsive plant means every
 * session start burns 6 SPI writes for nothing and stamps guard freshness it did not
 * earn). The kernel null (rfnm_qec) owns native sessions regardless. */
static int g_dc_null = 0;

/* antenna/path code -> librfnm path string (cfg->path convention: "embed","a".."h","loopback") */
static const char *path_code_str(int code) {
	switch (code) {
		case 0: return "embed";
		case 1: return "a"; case 2: return "b"; case 3: return "c"; case 4: return "d";
		case 5: return "e"; case 6: return "f"; case 7: return "g"; case 8: return "h";
		case 9: return "loopback";
		default: return "";
	}
}

/* RX path menu, enumerated from hwinfo + per-channel path_possible at rx init (see rfsp_paths).
 * The ws thread ships it at connect and re-ships whenever gen changes (path/channel applies,
 * INCLUDING rejected switches - clients must snap back to server truth). */
struct PathMenu {
	std::mutex mtx;
	char dgb_name[2][32] = {{0}, {0}};
	struct Entry { uint8_t code, channel, dgb; uint16_t fmin_mhz, fmax_mhz; };
	std::vector<Entry> entries;
	uint8_t cur_code = 0, cur_channel = 0;
	std::atomic<uint32_t> gen{0};            /* bumped on every menu/current change */
};
static PathMenu g_pathmenu;

/* selected (channel, path) survives daemon restarts: the watchdog/appd lifecycle restarts
 * spectrumd routinely, and a silent reset to the unit default read as "it changed my
 * antenna by itself" (2026-07-12 eve report) */
#define RTSA_PATH_CONF "/rfnm/spectrum/rtsa-path.conf"
static void path_persist_save(uint8_t channel, const char *path) {
	FILE *f = fopen(RTSA_PATH_CONF ".tmp", "w");
	if (!f) { return; }
	fprintf(f, "channel=%u\npath=%s\n", channel, path);
	fclose(f);
	rename(RTSA_PATH_CONF ".tmp", RTSA_PATH_CONF);
}
static bool path_persist_load(uint8_t *channel, char *path, size_t pathsz) {
	FILE *f = fopen(RTSA_PATH_CONF, "r");
	if (!f) { return false; }
	unsigned ch = 0;
	char p[24] = {0};
	bool ok = fscanf(f, "channel=%u\npath=%23s", &ch, p) == 2 && ch < 8 && p[0];
	fclose(f);
	if (ok) { *channel = static_cast<uint8_t>(ch); snprintf(path, pathsz, "%s", p); }
	return ok;
}

/* swept-analyzer mailbox (ws thread -> rx thread). mode flips the rx thread between the
 * real-time single-LO stream and run_sweep()'s hop ladder; gen bumps re-arm the ladder
 * when the user edits the range/bandwidth mid-sweep. */
struct SweepBox {
	std::atomic<int> mode{0};                /* 0 = real-time, 1 = sweep */
	std::atomic<uint64_t> start_hz{0};
	std::atomic<uint64_t> stop_hz{0};
	std::atomic<uint32_t> bw_hz{0};          /* fixed per-hop capture bandwidth (== sample rate) */
	std::atomic<uint32_t> gen{0};            /* bumped on any param change */
};

/* CyberEther Scale-block equivalent: the u8 quantization window, changeable at runtime.
 * Engines read these each batch; headers ship them so clients stay consistent. */
struct ScaleBox {
	std::atomic<int32_t> db_min_c{ -92 * 100 };   /* centi-dB for lock-free atomics */
	std::atomic<int32_t> db_max_c{ 98 * 100 };
};

/* ------------------------------------------------------------------ IQ recorder
 * Live-tap recorder: the rx thread copies each read into a bounded queue and a writer thread
 * drains it to the USB drive, so USB latency never stalls the FFT feed and there is NO device
 * handoff — you record exactly what the spectrum is showing (current LO / span / gain). Control
 * is a tiny HTTP GET server (see rec_http_server) behind nginx /api/app/rtsa/. */
#define REC_USB_DIR "/mnt/usb"
#define REC_QUEUE_MAX 48        /* reads buffered before the rx thread drops (counted, never blocks) */

struct Recorder {
	/* lifecycle: 0 idle, 1 recording, 2 done (incl. early-stopped, file saved), 3 error */
	std::atomic<int> state{0};
	std::atomic<uint64_t> got{0};        /* samples committed to disk */
	std::atomic<uint64_t> want{0};       /* target samples (0 until the writer arms) */
	std::atomic<uint64_t> dropped{0};    /* samples dropped because the writer fell behind */
	std::atomic<bool> active{false};     /* rx thread enqueues only while true */
	std::atomic<int> stop_req{0};        /* early-stop request from the http thread */

	/* live tuning published by the rx thread, snapshotted by the writer at record start */
	std::atomic<uint64_t> cur_freq{0};   /* Hz */
	std::atomic<uint32_t> cur_rate{0};   /* Hz */
	std::atomic<int32_t> cur_gain{0};
	char cur_path[24] = {0};             /* RF path, written once at rx init, read-only after */

	/* request params: set by the http thread before arming, read by the writer */
	double secs = 0;
	std::string name;
	int cf32 = 0;

	/* arm handshake: http thread -> writer thread */
	std::mutex start_mtx;
	std::condition_variable start_cv;
	bool start_flag = false;

	/* rx -> writer queue (bounded; a free list reuses buffers to avoid per-read malloc churn) */
	std::mutex qmtx;
	std::condition_variable qcv;
	std::deque<std::vector<int16_t>> filled;
	std::deque<std::vector<int16_t>> freebufs;

	std::mutex smtx;
	std::string summary;

	/* rx thread: copy one read into the queue if recording. Never blocks on I/O; drops if behind. */
	void push(const int16_t *iq, size_t elems) {
		if(!active.load(std::memory_order_relaxed)) { return; }
		std::vector<int16_t> b;
		{
			std::lock_guard<std::mutex> lk(qmtx);
			if(filled.size() >= REC_QUEUE_MAX) { dropped.fetch_add(elems, std::memory_order_relaxed); return; }
			if(!freebufs.empty()) { b = std::move(freebufs.back()); freebufs.pop_back(); }
			b.assign(iq, iq + 2 * elems);
			filled.push_back(std::move(b));
		}
		qcv.notify_one();
	}
};
static Recorder g_rec;

/* ------------------------------------------------------------------ swept output
 * run_sweep() (rx thread) assembles one full-span u8 row per sweep pass and publishes it
 * here; the ws pump ships it to sweep-mode clients. One row per pass (~a few Hz), so a
 * plain mutex + seq counter is ample — no lock-free ring needed. */
struct SweepOut {
	std::mutex mtx;
	uint8_t row[N_FFT] = {0};
	uint32_t seq = 0;                /* bumped once per completed sweep pass */
	double center_hz = 0, span_hz = 0;
	uint32_t nhops = 0;
};
static SweepOut g_sweep_out;

/* ------------------------------------------------------------------ shared rings */

#define SLOT_FREE 0
#define SLOT_FILLED 1
#define SLOT_BUSY 2
struct Slot {
	int16_t *buf;
	int engine;                 /* 0 = gpu (buf is CL-mapped, idx = gpu_idx), 1 = cpu */
	int gpu_idx;
	std::atomic<int> state{SLOT_FREE};
	uint64_t ticket = 0;        /* ring batch index; the rx producer claims it at FILL COMPLETION,
	                             * never at acquire - a half-filled slot must hold no ticket or
	                             * discarding it (park/sweep/reclock) wedges the in-order publish */
};
struct SlotPool {               /* single producer (rx), engines consume their kind */
	Slot slots[4];
	explicit SlotPool(GpuFFT *g, int cpu_workers) {
		for (int i = 0; i < 2; i++) { slots[i].buf = g->inMap[i]; slots[i].engine = 0; slots[i].gpu_idx = i; }
		for (int i = 2; i < 4; i++) {
			slots[i].buf = static_cast<int16_t *>(aligned_alloc(64, static_cast<size_t>(BATCH) * N_FFT * 4));
			slots[i].engine = cpu_workers > (i - 2) ? 1 : -1;   /* -1 = disabled */
		}
	}
	Slot *acquire_free() {       /* CPU-first: keep the slower engine's queue full, GPU soaks the rest */
		for (int i = 3; i >= 0; i--) {
			if (slots[i].engine >= 0 && slots[i].state.load(std::memory_order_acquire) == SLOT_FREE) { return &slots[i]; }
		}
		return nullptr;
	}
	Slot *take_filled(int engine) {
		for (auto &sl : slots) {
			if (sl.engine == engine && sl.state.load(std::memory_order_acquire) == SLOT_FILLED) {
				int exp = SLOT_FILLED;
				if (sl.state.compare_exchange_strong(exp, SLOT_BUSY)) { return &sl; }
			}
		}
		return nullptr;
	}
};

#define NBATCH_SLOTS (ROW_RING / BATCH)
struct RowRing {                         /* engines -> ws; out-of-order done, in-order publish */
	uint8_t *rows;
	std::atomic<uint64_t> published{0};  /* rows visible to WS (contiguous) */
	std::atomic<uint64_t> next_ticket{0};
	std::atomic<int> done[NBATCH_SLOTS];
	RowRing() {
		rows = static_cast<uint8_t *>(aligned_alloc(64, static_cast<size_t>(ROW_RING) * N_FFT));
		for (auto &d : done) { d = 0; }
	}
	uint8_t *row(uint64_t i) { return rows + (i % ROW_RING) * N_FFT; }
	/* engine finished batch `ticket`: mark and advance the watermark over completed prefix */
	void complete(uint64_t ticket) {
		done[ticket % NBATCH_SLOTS].store(1, std::memory_order_release);
		uint64_t p = published.load(std::memory_order_relaxed);
		while (done[(p / BATCH) % NBATCH_SLOTS].load(std::memory_order_acquire)) {
			done[(p / BATCH) % NBATCH_SLOTS].store(0, std::memory_order_relaxed);
			p += BATCH;
			published.store(p, std::memory_order_release);
		}
	}
	/* a ticket may only be reserved if its ring slice is clear of unread data */
	bool can_reserve(uint64_t ticket) { return ticket * BATCH < published.load(std::memory_order_acquire) + ROW_RING - BATCH; }
};

struct Stats {
	std::atomic<uint64_t> rows_produced{0}, rows_sent{0}, rows_dropped{0}, windows_skipped{0};
	std::atomic<uint32_t> gpu_us_per_fft{0};
	std::atomic<uint64_t> rx_samples{0};
	std::atomic<uint32_t> stream_errors{0};
	std::atomic<uint64_t> cpu_batches{0};
	std::atomic<int> clients{0};
	std::atomic<bool> rx_parked{false};        /* RX stream released because no WS client */
	std::atomic<uint32_t> demand_rows{3840};   /* max client-requested rows/s (ws thread updates) */
};

/* ------------------------------------------------------------------ sources */

static void synth_source(Cfg *cfg, SlotPool *sp, RowRing *rr, Stats *st) {
	uint32_t s = 0x52464e4d;
	auto xs = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
	double t = 0;
	while (!g_stop) {
		uint64_t tk = rr->next_ticket.load(std::memory_order_relaxed);
		Slot *sl = (rr->can_reserve(tk)) ? sp->acquire_free() : nullptr;
		if (!sl) { std::this_thread::sleep_for(std::chrono::microseconds(200)); continue; }
		sl->ticket = tk;
		rr->next_ticket.store(tk + 1, std::memory_order_relaxed);
		int16_t *dst = sl->buf;
		for (int r = 0; r < BATCH; r++) {
			double f0 = 0.18 + 0.10 * sin(t * 0.13);      /* drifting tone, fraction of fs */
			double ph = 0;
			double dph = 2.0 * M_PI * f0;
			int16_t *row = dst + static_cast<size_t>(r) * N_FFT * 2;
			for (int i = 0; i < N_FFT; i++) {
				int32_t nI = static_cast<int32_t>(xs() & 0x3FF) - 512;   /* noise floor */
				int32_t nQ = static_cast<int32_t>(xs() & 0x3FF) - 512;
				row[2 * i] = static_cast<int16_t>(nI + static_cast<int32_t>(6000 * cos(ph)));
				row[2 * i + 1] = static_cast<int16_t>(nQ + static_cast<int32_t>(6000 * sin(ph)));
				ph += dph;
			}
			t += 1.0 / 500;
		}
		sl->state.store(SLOT_FILLED, std::memory_order_release);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000 * BATCH / cfg->synth_rows));
	}
}

static ScaleBox *g_scale = nullptr;
static Cfg *g_cfg = nullptr;
static SlotPool *g_slots = nullptr;   /* watchdog telemetry only */
static Stats *g_stats = nullptr;      /* rec-http /clients liveness endpoint (appd polls it) */

#ifndef NO_LIBRFNM
/* shared sweep composition + per-hop passband flatness equalizer (both engines); SWEEP_USABLE
 * and SWEEP_DC_GUARD_HZ live there too */
#include "sweep_comp.h"
#define SWEEP_SCHED_MIN_BW 50000000u  /* >= this per-hop bw uses the exact schedule_rx engine; below it
                                       * the reorder stalls on fragmented windows, so use the drain engine.
                                       * Verified on Lime: 100M 23/24 & 50M 47/48 hops clean; 25M 21/95. */

/* RFIC LPF bandwidth for a given rate, every site (RT + both sweep engines). Stock = track
 * the rate (corner at Nyquist - the analog tail of the per-hop dome); the LPF
 * must NOT be left wide at low rates (an 80 MHz LPF at 25 MSPS aliases past the decimator's
 * finite stopband straight into the view). RFNM_RFIC_LPF_MHZ overrides for A/B experiments only. */
static int16_t rfic_lpf_for(double rate_hz) {
	static int ov = -2;   /* parse once: -1 = unset */
	if (ov == -2) {
		const char *e = getenv("RFNM_RFIC_LPF_MHZ");
		ov = -1;
		if (e) {
			int v = atoi(e);
			if (v >= 1 && v <= 160) { ov = v; }
		}
	}
	if (ov > 0) { return static_cast<int16_t>(ov); }
	int16_t lpf = static_cast<int16_t>(rate_hz / 1e6);
	return lpf < 1 ? 1 : lpf;
}

/* compose this pass and publish it as the next full-span sweep row */
static void sweep_publish(SweepComp &comp, Stats *st) {
	const float dbMin = g_scale->db_min_c.load(std::memory_order_relaxed) / 100.0f + 72.25f;   /* + GPU fp16 scale offset (cw_mag_u8 convention) */
	const float dbScale = 255.0f / (g_scale->db_max_c.load(std::memory_order_relaxed) / 100.0f - (dbMin - 72.25f));
	uint8_t rowbuf[N_FFT];
	comp.compose(rowbuf, dbMin, dbScale);
	{
		std::lock_guard<std::mutex> lk(g_sweep_out.mtx);
		memcpy(g_sweep_out.row, rowbuf, sizeof(rowbuf));
		g_sweep_out.center_hz = comp.center;
		g_sweep_out.span_hz = comp.span;
		g_sweep_out.nhops = static_cast<uint32_t>(comp.nhops());
		g_sweep_out.seq++;
	}
	st->rows_produced += 1;
}

/* Swept spectrum analyzer, DRAIN engine (low per-hop bandwidth: robust at EVERY rate, slower).
 * Hops the LO across [start,stop] at a fixed per-hop bandwidth: retune -> settle -> flush the pipe
 * by WALL-CLOCK time (the in-flight pipeline latency is ~fixed in TIME, so a fixed sample count
 * clears too little at high rates — 100M showed random 20 dB pass-to-pass swings; draining by
 * wall-clock clears the same time at any rate) -> capture one N_FFT window -> FFT on a private
 * pffft core -> POWER-detect each hop's central usable band into a full-span row. Reuses the SAME
 * running rx_stream as the real-time path (retune-while-streaming, no schedule_rx/raw-qbuf). This
 * engine drives the low-bandwidth sweeps where the schedule_rx engine's reorder stalls; the
 * schedule_rx engine (run_sweep_sched) is exact + faster and owns the high-bandwidth sweeps. */
#define SWEEP_SETTLE_US    500          /* Lime retune settling time (authoritative; validated on the
                                         * pipelined ladder: levels RT-exact at 500 us).
                                         * RFNM_SWEEP_SETTLE_US overrides for A/B experiments. */
#define SWEEP_FLUSH_CHUNK  65536        /* samples per discard read while draining */
#define SWEEP_FLUSH_MS     15           /* FALLBACK wall-clock drain when timestamps are unusable */

/* per-pass phase timing (RFNM_SWEEP_PROF=1): the speed work lives and dies by these numbers */
static bool g_sweep_prof = false;
struct SweepProf {
	double tune_ms = 0, settle_ms = 0, flush_ms = 0, cap_ms = 0, drain_ms = 0, fft_ms = 0;
	int flush_reads = 0, ts_gated = 0;
	int al_ok = 0, al_late = 0, al_mis = 0, al_fb = 0;   /* align_to_tick outcomes per pass */
	void log(const char *eng, size_t nhops, double pass_ms) const {
		if (!g_sweep_prof) { return; }
		logts("sweep-prof[%s]: pass %.0fms / %zu hops = %.1fms/hop | tune %.0f settle %.0f flush %.0f (%d reads, %d gated; align ok %d late %d mis %d fb %d) cap %.0f drain %.0f fft %.0f",
				eng, pass_ms, nhops, pass_ms / (nhops ? nhops : 1), tune_ms, settle_ms, flush_ms, flush_reads, ts_gated, al_ok, al_late, al_mis, al_fb, cap_ms, drain_ms, fft_ms);
	}
};
static double prof_ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
	return std::chrono::duration<double, std::milli>(b - a).count();
}

/* Per-hop sweep AGC: a gain table keyed by hop index, fed forward — hop k's gain is staged
 * into the SAME confirmed apply as its LO (zero extra transactions), and its captured window
 * updates the table for the next pass. A ring-side loop cannot do this job (no hop boundary
 * attribution at ~6 ms dwells); the channel's device agc field stays OFF for every spectrumd
 * session. Composition stays absolute: every staged delta is registered with
 * SweepComp::set_hop_gain_delta so the row is normalized back to the reference gain.
 * Thresholds mirror the kernel loop's, in the delivered CS16 domain (12-bit ADC << 4). */
#define SWEEP_AGC_ATT         23200    /* attack threshold, |I|/|Q| counts (~-3 dBFS) */
#define SWEEP_AGC_DEC          4150    /* decay threshold (~-18 dBFS) */
#define SWEEP_AGC_PIN         32600    /* ADC rail: attack harder */
#define SWEEP_AGC_HOLD_PASSES     8    /* consecutive passes below decay before stepping up */
#define SWEEP_AGC_UP_DB           2
struct SweepAgc {
	bool on = false;
	int ref = 0, gmin = 0, gmax = 0;
	std::vector<int> gain;               /* per-hop table, dB (next capture) */
	std::vector<uint8_t> low;            /* consecutive below-decay passes per hop */
	uint32_t steps_down = 0, steps_up = 0;
	std::chrono::steady_clock::time_point last_log;

	void init(rfnm::device &dev, uint32_t ch, const Cfg *cfg, size_t nhops) {
		on = cfg->agc && getenv("RFNM_SWEEP_AGC_OFF") == nullptr;
		ref = cfg->gain;
		gmin = ref; gmax = ref;
		const struct rfnm_api_rx_ch *rc = dev.get_rx_channel(ch);
		if (rc && static_cast<int>(rc->gain_range.max) > static_cast<int>(rc->gain_range.min)) {
			gmin = rc->gain_range.min;
			gmax = rc->gain_range.max;
			if (ref < gmin) { ref = gmin; }
			if (ref > gmax) { ref = gmax; }
		}
		gain.assign(nhops, ref);
		low.assign(nhops, 0);
		last_log = std::chrono::steady_clock::now();
	}
	/* ladder side, BEFORE hop k's apply: ride the tune's transaction */
	void stage(rfnm::device &dev, uint32_t ch, SweepComp &comp, size_t k) {
		if (!on) { return; }
		dev.set_rx_channel_gain(ch, static_cast<int8_t>(gain[k]), false);
		comp.set_hop_gain_delta(k, static_cast<float>(gain[k] - ref));
	}
	/* FFT-worker side, on hop k's captured window: two-rate step for the NEXT pass.
	 * The detector is peak-off-mean (AC), full IQ since the 2026-07-21 DC reform:
	 * - AC, not raw |sample|: residual analog DC does not scale with user gain, so a
	 *   raw-peak loop rails to gmin against it and achieves nothing.
	 * - Both lanes: the old I-ONLY exception existed because the un-nulled Lime Q DC
	 *   parked that lane at +32752 from mid-gain up and IQ leveling attacked the whole
	 *   span to gmin for nothing (A/B: floor +29 dB). The rfnm_lime DC guard now caps
	 *   the PGA so a DC rail is impossible by construction, and the null (rfnm_qec
	 *   kernel-side for native sessions, the dc-null client door here for local ones)
	 *   keeps both lanes centered - a lane the detector can finally trust.
	 *   --i-only-detector restores the workaround if a board proves otherwise.
	 * Decay is gated on the raw peak so it never steps INTO a rail. */
	void update(size_t k, const int16_t *iq, int n_elems) {
		if (!on || n_elems < 4) { return; }
		int64_t si = 0;
		int min_i = 32767, max_i = -32768;
		int min_q = 32767, max_q = -32768;
		int n = 0;
		for (int i = 0; i + 1 < n_elems; i += 2) {
			int vi = iq[i];
			int vq = iq[i + 1];
			if (vi == -32768 || vq == -32768) { continue; }   /* 0x8000 is unreachable from the 12-bit<<4 ADC path: fill/park sentinel, never data */
			si += vi;
			n++;
			if (vi < min_i) { min_i = vi; }
			if (vi > max_i) { max_i = vi; }
			if (g_iq_det) {
				if (vq < min_q) { min_q = vq; }
				if (vq > max_q) { max_q = vq; }
			}
		}
		if (n < 64) { return; }
		int mi = static_cast<int>(si / n);
		int ac = max_i - mi;
		if (mi - min_i > ac) { ac = mi - min_i; }
		int raw = max_i > -min_i ? max_i : -min_i;
		if (g_iq_det && max_q > min_q) {
			/* Q AC around the Q midpoint (no mean tracked for Q: min/max midpoint is the
			 * same peak-off-center witness at detector precision) */
			int cq = (max_q + min_q) / 2;
			int acq = max_q - cq;
			if (acq > ac) { ac = acq; }
			int rawq = max_q > -min_q ? max_q : -min_q;
			if (rawq > raw) { raw = rawq; }
		}
		if (ac >= SWEEP_AGC_ATT) {
			int down = 6;                                     /* 6 dB per octave of overshoot, 12 at the rail */
			if (ac >= SWEEP_AGC_PIN) { down = 12; }
			else { int o = ac / SWEEP_AGC_ATT; while (o > 1) { down += 6; o >>= 1; } }
			int g = gain[k] - down;
			if (g < gmin) { g = gmin; }
			if (g != gain[k]) { gain[k] = g; steps_down++; }
			low[k] = 0;
		} else if (ac && ac < SWEEP_AGC_DEC && raw < SWEEP_AGC_ATT) {
			if (++low[k] >= SWEEP_AGC_HOLD_PASSES) {
				low[k] = 0;
				if (gain[k] < gmax) { gain[k] += SWEEP_AGC_UP_DB; if (gain[k] > gmax) { gain[k] = gmax; } steps_up++; }
			}
		} else {
			low[k] = 0;
		}
	}
	/* pass-end: one compact line while the table is moving (never per-hop — log storms) */
	void pass_log() {
		if (!on || (!steps_down && !steps_up)) { return; }
		auto now = std::chrono::steady_clock::now();
		if (now - last_log < std::chrono::seconds(2)) { return; }
		last_log = now;
		int lo = gmax, hi = gmin;
		for (int g : gain) { if (g < lo) { lo = g; } if (g > hi) { hi = g; } }
		logts("sweep-agc: %u down %u up, gains [%d..%d] dB (ref %d)", steps_down, steps_up, lo, hi, ref);
		steps_down = 0; steps_up = 0;
	}
};

static void run_sweep_drain(rfnm::device &dev, std::unique_ptr<rfnm::rx_stream> &stream, rfnm::ch_helper &rx_ch,
		Cfg *cfg, SweepBox *sweep, Stats *st) {
	using namespace rfnm;
	uint32_t mygen = sweep->gen.load();
	double a = static_cast<double>(sweep->start_hz.load());
	double b = static_cast<double>(sweep->stop_hz.load());
	uint32_t bw = sweep->bw_hz.load();
	if (b <= a || bw < 1000000u) { sweep->mode.store(0); return; }   /* invalid range: bail to RT */
	{
		/* the antenna's limits are fixed: clamp the commanded range to the ACTIVE channel's
		 * tunable band (stale/legacy clients can command a range the path can't reach - the
		 * result was a wall of hold-fill shelves + tune rejects) */
		const struct rfnm_api_rx_ch *rc = dev.get_rx_channel(rx_ch.id);
		if (rc && rc->freq_max > rc->freq_min && rc->freq_max > 0) {
			if (a < static_cast<double>(rc->freq_min)) { a = static_cast<double>(rc->freq_min); }
			if (b > static_cast<double>(rc->freq_max)) { b = static_cast<double>(rc->freq_max); }
			if (b - a < 1e6) { logts("sweep: commanded range outside the path band - bailing to RT"); sweep->mode.store(0); return; }
		}
	}

	auto set_rate = [&](uint32_t r) -> bool {
		stream->stop();
		/* the reclock itself can fail transiently (park-resume + immediate sweep entry hit
		 * this on 2026-07-12: the silent failure left the DCS at the RT rate while the ladder
		 * believed the sweep bw - every tune of the pass rejected). Retry, and on persistent
		 * failure BAIL the sweep instead of sweeping a lie. */
		rfnm_api_failcode rr = RFNM_API_TIMEOUT;
		for (int i = 0; i < 4 && !g_stop; i++) {
			rr = dev.set_samp_rate(r, 20000000);
			if (rr == RFNM_API_OK) { break; }
			logts("sweep set_rate: set_samp_rate %.0fM failed (%d), retry %d", r / 1e6, rr, i + 1);
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
		if (rr == RFNM_API_OK) {
			cfg->rate = static_cast<double>(r);
			dev.set_rx_channel_rfic_lpf_bw(rx_ch.id, rfic_lpf_for(r), false);
			dev.apply(rx_ch.apply, true, 5000000);
		}
		/* patient restart with RECREATED stream objects: retrying start() on the same object
		 * after a reclock fails persistently (the 17:46 daemon death) - the sched-exit rebuild
		 * pattern is the proven one */
		int tries = 5;
		while (stream->start() != RFNM_API_OK && --tries && !g_stop) {
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			stream.reset(dev.rx_stream_create(rx_ch.mask));
			stream->set_auto_dc_offset(false);
		}
		if (!tries) { logts("sweep set_rate: stream restart failed after retries"); g_stop = 1; return false; }
		return rr == RFNM_API_OK;
	};
	if (static_cast<double>(bw) != cfg->rate) {
		if (!set_rate(bw)) {
			if (!g_stop) { logts("sweep: reclock to bw failed - bailing to RT"); sweep->mode.store(0); }
			return;
		}
	}

	SweepComp comp(a, b, bw);
	SweepAgc agc;
	agc.init(dev, rx_ch.id, cfg, comp.nhops());
	logts("sweep: %.1f-%.1f MHz, bw %.2f MSPS, %zu hops (drain%s)", a / 1e6, b / 1e6, bw / 1e6, comp.nhops(), agc.on ? ", agc" : "");

	std::vector<int16_t> cap(static_cast<size_t>(SWEEP_FLUSH_CHUNK) * 2);
	size_t elems; uint64_t ts;
	uint32_t settle_us = SWEEP_SETTLE_US;
	if (const char *e = getenv("RFNM_SWEEP_SETTLE_US")) { int v = atoi(e); if (v >= 150 && v <= 20000) { settle_us = static_cast<uint32_t>(v); } }
	uint64_t settle_ticks;
	{
		struct timing swt = {};
		dev.get_timing(DIR_RX, &swt);
		settle_ticks = (swt.tick_hz ? swt.tick_hz : 61440000ull) * settle_us / 1000000ull;
	}
	int wedge_hops = 0;
	int rearms_total = 0;   /* hard cap: repeated stream churn is a kernel-thrash amplifier
	                         * (the 07-12 watchdog reboot happened under exactly this class) */
	auto last_rearm = std::chrono::steady_clock::now() - std::chrono::seconds(10);
	/* reorder priming: right after ANY stream start the librfnm recombiner needs a burst of
	 * contiguous consumption to adopt cc continuity (CCINIT wants ~10 buffers). The dry-break
	 * flush stops at the live edge and starves it -> deterministic cc-timeout wedge at sweep
	 * entry (found the hard way: iteration 2/3 wedged where the dumb 15 ms flush survived,
	 * because the dumb flush primed CCINIT by accident). ~12 full chunks or 60 ms, then dry
	 * logic is safe. */
	auto prime_stream = [&]() {
		void *bufs[1] = { cap.data() };
		auto pdl = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
		int full = 0;
		while (full < 12 && !g_stop && std::chrono::steady_clock::now() < pdl) {
			if (stream->read(bufs, SWEEP_FLUSH_CHUNK, elems, ts, 10000) == RFNM_API_OK && elems >= SWEEP_FLUSH_CHUNK / 2) { full++; }
		}
	};
	prime_stream();


	/* per-hop capture buffers + FFT worker: comp.add_hop is ~0.3 ms of pure compute per hop
	 * and used to run inline in the ladder (20 ms of every pass); the worker overlaps it. */
	const size_t nhops_d = comp.nhops();
	std::vector<std::vector<int16_t>> hopbuf(nhops_d, std::vector<int16_t>(static_cast<size_t>(N_FFT) * 2));
	std::mutex fq_mtx;
	std::condition_variable fq_cv;
	std::deque<size_t> fq;
	size_t fft_queued = 0, fft_done = 0;
	bool fq_stop = false;
	std::thread fft_thr([&]() {
		pin_self(1);
		std::unique_lock<std::mutex> lk(fq_mtx);
		for (;;) {
			fq_cv.wait(lk, [&] { return fq_stop || !fq.empty(); });
			if (fq.empty()) { if (fq_stop) { return; } continue; }
			size_t k = fq.front(); fq.pop_front();
			lk.unlock();
			agc.update(k, hopbuf[k].data(), 2 * N_FFT);   /* feed-forward: steps hop k's TABLE gain for the next pass */
			comp.add_hop(k, hopbuf[k].data());
			lk.lock();
			fft_done++;
			fq_cv.notify_all();
		}
	});

	/* pipelined ladder: hop k+1's confirmed apply is issued as soon as hop k's capture span
	 * has PASSED ON THE ADC (gate + N_FFT samples + guard); its samples are then already in
	 * flight down the pipe while the apply executes, and align_to_tick(gate_k) + read only
	 * have to wait out the residual delivery lag. The retune can never corrupt hop k: the
	 * apply is issued after the capture span, and the command transit adds margin on top.
	 * RFNM_SWEEP_PIPE_US (default 100) credits the minimum observed issue->exec latency. */
	const uint64_t cap_ticks = dev.rx_samples_to_ticks(N_FFT);
	uint32_t pipe_us = 100;
	if (const char *e = getenv("RFNM_SWEEP_PIPE_US")) { int v = atoi(e); if (v >= 0 && v <= 400) { pipe_us = static_cast<uint32_t>(v); } }
	struct timing swt2 = {};
	dev.get_timing(DIR_RX, &swt2);
	const uint64_t tick_hz_d = swt2.tick_hz ? swt2.tick_hz : 61440000ull;
	const uint64_t guard_ticks = 30ull * tick_hz_d / 1000000ull;
	const uint64_t credit_ticks = static_cast<uint64_t>(pipe_us) * tick_hz_d / 1000000ull;

	/* deterministic-reject memory: a hop whose tune keeps rejecting (#32 dead SXR LOs) is
	 * skipped after 3 consecutive rejects and re-probed every 16th pass — narrow zoomed
	 * sweeps run 30+ passes/s, and re-tuning a known-dead LO every pass was a librfnm
	 * error-log storm (~60 lines/s) plus 10 ms/pass of wasted ladder time. Hold-fill
	 * covers the display exactly as for any missed hop. */
	std::vector<uint8_t> rej_streak(nhops_d, 0);
	uint32_t pass_no = 0;

	while (sweep->mode.load(std::memory_order_relaxed) && !g_stop && sweep->gen.load() == mygen) {
		auto tp_pass = std::chrono::steady_clock::now();
		pass_no++;
		SweepProf pr;
		comp.begin_pass();
		{ std::lock_guard<std::mutex> lk(fq_mtx); fq.clear(); fft_queued = 0; fft_done = 0; }
		g_cfg->db_min = g_scale->db_min_c.load(std::memory_order_relaxed) / 100.0f;
		g_cfg->db_max = g_scale->db_max_c.load(std::memory_order_relaxed) / 100.0f;

		/* host tick map: one clock_now per pass, "device now" from the host clock after
		 * (drift < 10 us over a pass). clock_now extends into the stream stamp domain. */
		uint64_t tick0 = 0;
		dev.clock_now(&tick0);
		auto map0 = std::chrono::steady_clock::now();
		auto nowt = [&]() -> uint64_t {
			double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - map0).count();
			return tick0 + static_cast<uint64_t>(el * static_cast<double>(tick_hz_d));
		};
		auto tune_confirmed = [&](size_t k) -> bool {
			if (rej_streak[k] >= 3 && (pass_no & 63) != 0) { return false; }   /* known-dead LO: hold-fill, re-probe every 64th pass
			                                                                    * (at the ~130 ms full-span pass this is ~8 s between
			                                                                    * librfnm reject-log triplets; 16 was a 2 s log metronome) */
			auto tk0 = std::chrono::steady_clock::now();
			agc.stage(dev, rx_ch.id, comp, k);   /* hop gain rides the SAME apply as the LO */
			dev.set_rx_channel_freq(rx_ch.id, static_cast<int64_t>(comp.los[k]), false);
			rfnm_api_failcode tr;
			auto rdl = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
			for (;;) {
				tr = dev.apply(rx_ch.apply, true, 1000000);
				/* retry only TRANSIENT outcomes (cmd-busy/transport). A device REJECT (e.g. the
				 * #32 SXR gap's "Tune failed") is deterministic - retrying it 30x per hop per
				 * pass was a librfnm error-log storm and pure wasted pass time. */
				if (tr == RFNM_API_OK || (tr != RFNM_API_TIMEOUT && tr != RFNM_API_USB_FAIL)) { break; }
				if (std::chrono::steady_clock::now() >= rdl || g_stop) { break; }
				std::this_thread::sleep_for(std::chrono::microseconds(300));
			}
			pr.tune_ms += prof_ms(tk0, std::chrono::steady_clock::now());
			if (tr != RFNM_API_OK) {
				st->stream_errors++;
				if (rej_streak[k] < 255) { rej_streak[k]++; }
				return false;
			}
			rej_streak[k] = 0;
			g_rec.cur_freq.store(static_cast<uint64_t>(comp.los[k]), std::memory_order_relaxed);
			return true;
		};

		uint64_t gate_k = 0;
		bool have_k = tune_confirmed(0);
		if (have_k) { gate_k = nowt() + settle_ticks; }
		for (size_t k = 0; k < nhops_d && sweep->mode.load(std::memory_order_relaxed) && !g_stop; k++) {
			uint64_t gate_next = 0;
			bool have_next = false;
			if (k + 1 < nhops_d) {
				if (have_k) {
					/* hop k's capture span must have passed on the ADC before the retune lands */
					uint64_t safe = gate_k + cap_ticks + guard_ticks - credit_ticks;
					int64_t rem = static_cast<int64_t>(safe - nowt());
					if (rem > 0) { std::this_thread::sleep_for(std::chrono::microseconds(static_cast<uint64_t>(rem) * 1000000ull / tick_hz_d + 1)); }
				}
				have_next = tune_confirmed(k + 1);
				if (have_next) { gate_next = nowt() + settle_ticks; }
			}
			if (have_k) {
				auto tp1 = std::chrono::steady_clock::now();
				/* phytimer-scheduled flush: align_to_tick consumes-and-drops until the next
				 * delivered sample is stamped at the gate - sample-exact, stamp-domain (no
				 * run-until-dry heuristics). OK/LATE/MISALIGNED are all terminal.
				 * Only data starvation falls back to a bounded dry-drain. */
				rfnm_api_failcode ar = stream->align_to_tick(gate_k, SWEEP_FLUSH_MS * 1000);
				if (ar == RFNM_API_OK) { pr.al_ok++; pr.ts_gated++; }
				else if (ar == RFNM_API_SCHED_LATE) { pr.al_late++; pr.ts_gated++; }
				else if (ar == RFNM_API_SCHED_MISALIGNED) { pr.al_mis++; pr.ts_gated++; }
				else {
					pr.al_fb++;
					void *bufs[1] = { cap.data() };
					auto fdl = std::chrono::steady_clock::now() + std::chrono::milliseconds(SWEEP_FLUSH_MS);
					while (!g_stop && std::chrono::steady_clock::now() < fdl) {
						if (stream->read(bufs, SWEEP_FLUSH_CHUNK, elems, ts, 1000) == RFNM_API_OK && elems >= SWEEP_FLUSH_CHUNK) {
							pr.flush_reads++;
							continue;
						}
						break;
					}
				}
				auto tp2 = std::chrono::steady_clock::now();
				pr.flush_ms += prof_ms(tp1, tp2);
				size_t have = 0;
				bool bad = false;
				while (have < static_cast<size_t>(N_FFT) && !g_stop) {
					void *bp[1] = { hopbuf[k].data() + have * 2 };
					size_t got = 0;
					if (stream->read(bp, N_FFT - have, got, ts, 200000) != RFNM_API_OK) { bad = true; break; }
					have += got;
				}
				pr.cap_ms += prof_ms(tp2, std::chrono::steady_clock::now());
				if (bad || have < static_cast<size_t>(N_FFT)) {
					st->stream_errors++;
					/* wedge watchdog: two consecutive dead hops = the reorder cc-stalled (the
					 * known churn wedge, G15 class). A stream stop/start re-arms continuity in
					 * ~50 ms; without it the whole session decays to 200 ms-timeout zero rows. */
					if (++wedge_hops >= 2 && std::chrono::steady_clock::now() - last_rearm > std::chrono::milliseconds(1500)) {
						if (++rearms_total > 3) {
							logts("sweep: %d re-arms didn't clear the wedge - bailing to RT", rearms_total - 1);
							sweep->mode.store(0);
							break;
						}
						stream->stop();
						if (stream->start() != RFNM_API_OK) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); stream->start(); }
						prime_stream();   /* re-arm CCINIT or the restart just re-wedges */
						dev.clock_now(&tick0);   /* the extended-tick domain re-anchored: rebuild the map */
						map0 = std::chrono::steady_clock::now();
						last_rearm = std::chrono::steady_clock::now();
						wedge_hops = 0;
						pr.drain_ms += 1;   /* prof: count re-arms in the drain column */
						have_next = false;  /* its gate is from the old domain: skip, hold-fill covers it */
					}
				} else {
					wedge_hops = 0;
					st->rx_samples += have;
					std::lock_guard<std::mutex> lk(fq_mtx);
					fq.push_back(k);
					fft_queued++;
					fq_cv.notify_all();
				}
			}
			gate_k = gate_next;
			have_k = have_next;
		}
		{
			std::unique_lock<std::mutex> lk(fq_mtx);
			fq_cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return fft_done == fft_queued; });
		}
		sweep_publish(comp, st);
		agc.pass_log();
		pr.log("drain", comp.nhops(), prof_ms(tp_pass, std::chrono::steady_clock::now()));
		/* pass pacing: a 3-4 hop zoomed sweep completes in ~15 ms - free-running that is
		 * 200+ LO retunes/s for a refresh nobody can see. Cap at ~30 passes/s. */
		std::this_thread::sleep_until(tp_pass + std::chrono::milliseconds(33));
	}

	{
		std::lock_guard<std::mutex> lk(fq_mtx);
		fq_stop = true;
		fq_cv.notify_all();
	}
	fft_thr.join();
	/* NO RT restore here: a gen re-arm (zoom-following sweep range) re-enters immediately with
	 * the same bw — restoring the RT rate per re-arm was two reclocks per zoom step. The RT
	 * restore lives in rx_source's sweep wrapper and runs once, on the sweep -> RT edge. */
}


/* Swept spectrum analyzer, schedule_rx / phytimer-ladder engine. Runs a PRIVATE scheduled-capture
 * RX session: the real-time rx_stream is destroyed before entry and rebuilt on exit (see rx_source).
 *
 * The ladder runs at the retune's physical floor: confirmed apply (the confirm IS the "FE
 * programmed" edge; deadline-retried through the anchor-TDD cmd-busy bursts) -> schedule the hop
 * window at now+settle (0.5 ms, the authoritative Lime retune settling time) -> consume-and-
 * attribute delivered buffers UNTIL the window closes -> next hop. No wall-clock waits, no
 * confirm-wait beyond the apply itself, no phytimer poll loops: "now" comes from ONE clock_now
 * per pass extended by the host monotonic clock (crystal offset over a ~100 ms pass is < 10 us,
 * and the map is re-anchored every pass).
 *
 * Attribution is by stamp: every dqbuf'd packet lands in the hop whose window tick-span contains
 * its phytimer (raw u32 compare, spans << 2^32). Per-adc delivery is cc-ordered = stamp-ordered,
 * so a moving lower bound walks the ladder once per pass. Filled hops are FFT'd on a worker
 * thread (comp.add_hop) that overlaps the ladder; the pass ends with a bounded straggler tail
 * (idle-exit), not a flat deadline. Hops that miss (dead SXR LOs 1550/1590, apply refusals,
 * VSPA short-fills) are hold-filled by the composer exactly as before.
 *
 * The anchor TDD still runs (the VSPA pattern word is stamp-chain-critical, and it
 * mints/keeps the epoch) but SPARSE: 8 duty chunks per period instead of sweep_qual's 400-chunk
 * flood. The flood's original job - pushing the reorder past its gap threshold - is obsolete:
 * librfnm now adopts current-epoch DISCONT-flagged packets immediately, and every scheduled
 * window boundary ships one. Sparse duty also shrinks the M4 cmd-busy bursts that used to refuse
 * mid-ladder applies for ~4 ms per period. */
#define SWQ_TPC           768u        /* ADC chunk (samples); scheduled windows are whole chunks */
#define SWQ_WIN_CHUNK     22u         /* default window = 22*768 = 16896 samples REQUESTED. The fw fill
                                       * contract, measured sample-exactly 2026-07-12 (hop-buffer dump,
                                       * spectra of each half): a C-chunk scheduled window ships C*384
                                       * samples, and only the FIRST C*192 of those are real full-rate
                                       * RF from the window open - the back half of the shipped buffer
                                       * is parked-lane DC garbage (a huge DC constant, occasionally
                                       * railed full-scale). The "windows deliver half" symptom was
                                       * this, compounded: real content = request/4. Any assembly that
                                       * reaches past C*192 mixes park-DC into the FFT - that was the
                                       * per-hop +-20 dB level lottery of every scheduled-engine trace.
                                       * 22 chunks = 4224 real samples >= N_FFT + margin. The old
                                       * 40-chunk engine survived by accident (40*192 = 7680 > 4096).
                                       * RFNM_SWEEP_WIN_CHUNKS / RFNM_SWEEP_SKIP_US override. */
#define SWQ_SKIP_US       0u          /* delivered samples discarded from the real region's head (none:
                                       * the real region is clean from sample 0 at settle >= 500 us) */
#define SWQ_SETTLE_US     500u        /* LO settle scheduled between apply-confirm and window open
                                       * (authoritative Lime figure; RFNM_SWEEP_SETTLE_US overrides) */
#define SWQ_TAIL_MS       60u         /* straggler tail cap per pass (idle-exits early) */
#define SWQ_ANCHOR_DUTY   8u          /* sparse anchor TDD duty, chunks (period = 240 chunks) */

static void run_sweep_sched(rfnm::device &dev, rfnm::ch_helper &rx_ch, Cfg *cfg, SweepBox *sweep, Stats *st) {
	using namespace rfnm;
	uint32_t mygen = sweep->gen.load();
	double a = static_cast<double>(sweep->start_hz.load());
	double b = static_cast<double>(sweep->stop_hz.load());
	uint32_t bw = sweep->bw_hz.load();
	if (b <= a || bw < 1000000u) { sweep->mode.store(0); return; }   /* invalid range: bail to RT */

	double entry_rate = cfg->rate, entry_freq = cfg->freq;
	auto restore_rt = [&]() {
		/* leave the device where rx_source's rx_stream_create expects it (pre-stream RT state) */
		if (cfg->rate != entry_rate) {
			dev.set_samp_rate(static_cast<uint32_t>(entry_rate), 20000000);
			cfg->rate = entry_rate;
			dev.set_rx_channel_rfic_lpf_bw(rx_ch.id, rfic_lpf_for(entry_rate), false);
		}
		dev.set_rx_channel_freq(rx_ch.id, static_cast<int64_t>(entry_freq), false);
		dev.set_rx_channel_status(rx_ch.id, RFNM_CH_RF_OFF, RFNM_CH_STREAM_AUTO, false);
		dev.apply(rx_ch.apply, true, 1000000);
		cfg->freq = entry_freq;
		g_rec.cur_freq.store(static_cast<uint64_t>(entry_freq), std::memory_order_relaxed);
		g_rec.cur_rate.store(static_cast<uint32_t>(entry_rate), std::memory_order_relaxed);
	};

	/* ---- private scheduled-RX session bring-up (mirror sweep_qual) ---- */
	if (dev.schedule_reset()) { logts("sweep: schedule_reset fail"); sweep->mode.store(0); restore_rt(); return; }
	dev.set_samp_rate(bw, 20000000);
	cfg->rate = static_cast<double>(bw);
	dev.set_rx_channel_status(rx_ch.id, RFNM_CH_RF_ON, RFNM_CH_STREAM_ON, false);
	dev.set_rx_channel_freq(rx_ch.id, static_cast<int64_t>(a), false);
	dev.set_rx_channel_gain(rx_ch.id, cfg->gain, false);
	if (!cfg->path.empty()) { dev.set_rx_channel_path(rx_ch.id, device::string_to_rf_path(cfg->path), false); }
	dev.set_rx_channel_rfic_lpf_bw(rx_ch.id, rfic_lpf_for(bw), false);
	if (dev.apply(rx_ch.apply, true, 5000000)) { logts("sweep: rx apply fail"); sweep->mode.store(0); restore_rt(); return; }

	struct timing rt_pre = {};
	dev.get_timing(DIR_RX, &rt_pre);
	uint32_t e_pre = rt_pre.epoch;

	/* anchor bootstrap: the TDD grid mints/keeps the phytimer epoch and feeds the M4 walker's
	 * pattern word (scheduled sessions REQUIRE a TDD pattern). SPARSE duty - the
	 * old 400-chunk flood is obsolete (see the engine comment) and its cmd-busy bursts were
	 * the mid-ladder apply refusals. The period is stretched to ~0.9 s: the pattern's OWN duty
	 * edges re-gate the RX lane, and any edge landing inside a hop window splits the capture
	 * into two gain states with rail garbage at the seam (measured: two 3072-sample halves at
	 * different rms, full-scale DC at the boundary, one corrupted hop per ~3 at a 3.69 ms
	 * period). At ~0.9 s an edge clips at most one hop per ~9 passes and the median-of-passes
	 * absorbs it. */
	{
		/* re-reset the ring right before the pattern: an apply with the ring armed lets the
		 * kernel push its own quiesce entries (ring "dirty"), and a dirty ring REFUSES SET_TDD
		 * (the #29 mixed-verbs guard). A reset immediately before leaves no window to dirty. */
		dev.schedule_reset();
		uint64_t chunk_ticks = dev.rx_samples_to_ticks(SWQ_TPC);
		uint32_t duty_chunks = SWQ_ANCHOR_DUTY;
		if (const char *e = getenv("RFNM_SWEEP_ANCHOR_DUTY")) { duty_chunks = static_cast<uint32_t>(atoi(e)); }
		if (duty_chunks < 1) { duty_chunks = 1; }
		if (duty_chunks > 200) { duty_chunks = 200; }
		/* period cap: librfnm validates the pattern in its own chunk unit (384 << rx_dcs_div
		 * ticks) against a u16 count, and hwinfo's rx_dcs_div can read 0 here (chain state
		 * refreshes on stream start) - the safe cap is 65535*384 ticks ~= 0.25 s. 16000 of our
		 * chunks (~0.246 s at 50M) fits; one duty edge per ~2.3 passes, median-rejected. */
		uint32_t period_chunks = 16000;
		if (const char *e = getenv("RFNM_SWEEP_ANCHOR_PERIOD")) { int v = atoi(e); if (v >= 240 && v <= 16000) { period_chunks = static_cast<uint32_t>(v); } }
		rfnm_api_failcode tr = dev.rx_tdd_configure(static_cast<uint64_t>(period_chunks) * chunk_ticks, static_cast<uint64_t>(duty_chunks) * chunk_ticks);
		if (tr) {
			logts("sweep: anchor (rx_tdd_configure) fail %d (period %u duty %u chunk_ticks %llu)", tr, period_chunks, duty_chunks, (unsigned long long)chunk_ticks);
			sweep->mode.store(0); restore_rt(); return;
		}
	}

	/* Reuse the device's PERSISTENT buffer pool. The RT rx_stream already allocated MIN_RX_BUFCNT
	 * device-owned rx_bufs (rx_work_start only allocates when qbuf_cnt==0), and destroying the RT
	 * stream leaves them queued with qbuf_cnt intact. We must NOT queue our own buffers here: the
	 * device stores the raw rx_buf* pointers, so any pool we free on exit would dangle in rx_s.in
	 * and corrupt the rebuilt RT stream (that was the `corrupted size vs. prev_size` heap abort). */
	dev.rx_work_start();

	/* wait for the fresh anchored epoch, then flush the anchor burst backlog (usb_cc -> accept-any) */
	struct timing rt = {};
	for (int tries = 0; tries < 2000; tries++) {
		dev.get_timing(DIR_RX, &rt);
		if (rt.anchored && rt.epoch != e_pre) { break; }
		std::this_thread::sleep_for(std::chrono::microseconds(500));
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	{ rx_buf *bp = nullptr; while (dev.rx_dqbuf(&bp, rx_ch.mask, 0) == RFNM_API_OK) { dev.rx_qbuf(bp); } }
	logts("sweep: anchored t0=%llu epoch=%u R=%u/%u tick_hz=%llu",
			(unsigned long long)rt.t0_ext, rt.epoch, rt.r_num, rt.r_den, (unsigned long long)rt.tick_hz);

	uint32_t win_chunks = SWQ_WIN_CHUNK;
	if (const char *e = getenv("RFNM_SWEEP_WIN_CHUNKS")) { int v = atoi(e); if (v >= 6 && v <= 120) { win_chunks = static_cast<uint32_t>(v); } }
	uint32_t settle_us = SWQ_SETTLE_US;
	if (const char *e = getenv("RFNM_SWEEP_SETTLE_US")) { int v = atoi(e); if (v >= 150 && v <= 20000) { settle_us = static_cast<uint32_t>(v); } }
	uint32_t skip_us = SWQ_SKIP_US;
	if (const char *e = getenv("RFNM_SWEEP_SKIP_US")) { int v = atoi(e); if (v >= 0 && v <= 2000) { skip_us = static_cast<uint32_t>(v); } }
	/* optional extra head-drop within the real region (full-rate samples) */
	const size_t skip_smp = ((static_cast<uint64_t>(skip_us) * bw / 1000000ull) + 127) & ~127ull;
	{
		/* real content = 192 samples per requested chunk: the kept [skip, skip+N_FFT) region
		 * must fit entirely inside it or park-DC pollutes the FFT */
		uint32_t need = static_cast<uint32_t>((skip_smp + N_FFT + 191) / 192);
		if (win_chunks < need) { win_chunks = need; }
	}
	const uint32_t win_samples  = win_chunks * SWQ_TPC;
	const uint32_t win_ticks    = static_cast<uint32_t>(dev.rx_samples_to_ticks(win_samples));   /* window span in ticks */
	const uint64_t tick_hz      = rt.tick_hz ? rt.tick_hz : 61440000ull;
	const uint64_t settle_ticks = static_cast<uint64_t>(settle_us) * tick_hz / 1000000ull;

	/* ---- hop plan + shared composition/equalizer (identical detector to the drain engine) ---- */
	SweepComp comp(a, b, bw);
	const size_t nhops = comp.nhops();
	SweepAgc agc;
	agc.init(dev, rx_ch.id, cfg, nhops);
	logts("sweep: %.1f-%.1f MHz, bw %.2f MSPS, %zu hops, win %u samp (%u ticks), settle %u us (schedule_rx ladder%s)",
			a / 1e6, b / 1e6, bw / 1e6, nhops, win_samples, win_ticks, settle_us, agc.on ? ", agc" : "");

	/* per-hop scheduled window ticks (u32) + N_FFT sample assembly, allocated once for the session */
	std::vector<uint32_t> htick(nhops, 0);
	std::vector<std::vector<int16_t>> hopbuf(nhops, std::vector<int16_t>(static_cast<size_t>(N_FFT) * 2));
	std::vector<size_t> hophave(nhops, 0);       /* KEPT samples assembled (post-skip) */
	std::vector<size_t> hopseen(nhops, 0);       /* raw delivered samples consumed (incl. the skip) */
	std::vector<uint8_t> hopdone(nhops, 0);      /* 1 = filled to N_FFT, 2 = missed (apply/schedule fail) */
	/* window SLOTS in schedule order (pre-roll, hops, sentinels): attribution works on slots
	 * because of the session stamp shift below - a buffer stamped in slot s carries slot s-K's
	 * capture, and for the ladder tail that stamp lands in a SENTINEL slot */
	std::vector<uint32_t> slot_tick; slot_tick.reserve(nhops + 8);
	std::vector<int> slot_hop; slot_hop.reserve(nhops + 8);      /* hop index, -1 = throwaway */
	size_t hops_sched = 0;

	/* ---- session stamp-shift calibration (VSPA stamp-queue drift residual) ----
	 * The VSPA anchors each scheduled window's stamp chain from its own 16-deep queue; entries
	 * fed for windows that never emit (boundary losses at session start) leave the queue offset,
	 * and EVERY subsequent window's data ships under the tick of the window K slots later. The
	 * stamps stay self-consistent, so the composed row is the true spectrum displaced by K hops
	 * (~40 MHz per hop at 50M) - the per-session "level lottery" (measured K = 0,1,3 across
	 * back-to-back sessions; sweep_qual's ok=1/3 is the same disease). Until the fw per-window
	 * handshake lands, measure K per session with a rotation-unique hi/lo GAIN code ladder and
	 * correct attribution by K. Ambient-independent: the 30 dB gain steps dominate any signal. */
	/* ---- slot identity: LENGTH-CODED windows (VSPA stamp-queue drift residual) ----
	 * The VSPA anchors scheduled-window stamp chains from its own 16-deep queue; entries fed
	 * for windows that never emit leave the queue offset, and subsequent windows' data ships
	 * under a NEIGHBORING window's tick. Measured: composed rows displaced by a whole number
	 * of hops, per session AND piecewise within a pass (freq-coded probes decoded exact shifts
	 * of -1, 0, +1, +3). Stamps therefore CANNOT identify which window a buffer belongs to.
	 * The delivered length can: elem_cnt == requested_chunks * 384 exactly, so scheduling the
	 * ladder with a mod-4 chunk-count code (base+0..3) makes every window self-identifying up
	 * to +-1 slot, and an ordinal decoder (windows ship in capture order) pins it completely. */
	auto slotC = [&](size_t i) -> uint32_t { return win_chunks + static_cast<uint32_t>(i % 4); };
	const uint64_t chunk_ticks_w = dev.rx_samples_to_ticks(SWQ_TPC);

	/* FFT worker: comp.add_hop is ~0.3 ms/hop of pure compute - run it off the ladder thread so
	 * capture never stalls on it. The ladder owns comp again after the per-pass barrier. */
	std::mutex fq_mtx;
	std::condition_variable fq_cv;
	std::deque<size_t> fq;
	size_t fft_queued = 0, fft_done = 0;
	bool fq_stop = false;
	std::thread fft_thr([&]() {
		pin_self(1);
		std::unique_lock<std::mutex> lk(fq_mtx);
		for (;;) {
			fq_cv.wait(lk, [&] { return fq_stop || !fq.empty(); });
			if (fq.empty()) { if (fq_stop) { return; } continue; }
			size_t k = fq.front(); fq.pop_front();
			lk.unlock();
			agc.update(k, hopbuf[k].data(), 2 * N_FFT);   /* feed-forward: steps hop k's TABLE gain for the next pass */
			comp.add_hop(k, hopbuf[k].data());
			lk.lock();
			fft_done++;
			fq_cv.notify_all();
		}
	});
	auto fft_push = [&](size_t k) {
		std::lock_guard<std::mutex> lk(fq_mtx);
		fq.push_back(k);
		fft_queued++;
		fq_cv.notify_all();
	};

	while (sweep->mode.load(std::memory_order_relaxed) && !g_stop && sweep->gen.load() == mygen) {
		auto tp_pass = std::chrono::steady_clock::now();
		SweepProf pr;
		comp.begin_pass();
		std::fill(hophave.begin(), hophave.end(), 0u);
		std::fill(hopseen.begin(), hopseen.end(), 0u);
		std::fill(hopdone.begin(), hopdone.end(), 0u);
		slot_tick.clear();
		slot_hop.clear();
		hops_sched = 0;
		size_t done = 0;          /* hops filled to N_FFT */
		{ std::lock_guard<std::mutex> lk(fq_mtx); fft_queued = 0; fft_done = 0; }
		g_cfg->db_min = g_scale->db_min_c.load(std::memory_order_relaxed) / 100.0f;
		g_cfg->db_max = g_scale->db_max_c.load(std::memory_order_relaxed) / 100.0f;

		/* Reset the reorder's expected usb_cc to accept-anything before each pass: the inter-pass
		 * cc gap is far larger than RX_RECOMB_BUF_LEN and the first fresh window's DISCONT packet
		 * re-adopts instantly. Stamp attribution is unaffected (raw phytimer). */
		dev.rx_flush(0, rx_ch.mask);
		/* NO schedule_reset here: the walker consumes the gen bump lazily (at the next doorbell),
		 * and by then this pass's first window is already queued - cons=prod swallowed it (hop 0
		 * delivered nothing every pass). Stale prior-pass requests age out as late-drops instead,
		 * and attribution is stamp-exact so their ghosts cannot pollute this pass. */

		/* one control round trip per pass: anchor the tick<->host-monotonic map. Everything else
		 * derives "device now" from the host clock (drift < 10 us over a pass, re-anchored here). */
		uint64_t tick0 = 0;
		dev.clock_now(&tick0);
		const auto map0 = std::chrono::steady_clock::now();
		auto now_ticks = [&]() -> uint64_t {
			double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - map0).count();
			return tick0 + static_cast<uint64_t>(el * static_cast<double>(tick_hz));
		};

		/* Ordinal + length-code decoder. Windows EMIT (and ship) in capture order; a scheduled
		 * window's buffer carries DISCONT (bit0) while the sparse-anchor duty buffers do not
		 * (the pattern stamp-chains across its gaps by design), and elem_cnt == chunks*384
		 * exactly - so each window self-identifies by length code and the ordinal cursor pins
		 * the slot without trusting the (session/piecewise-shifted) stamps at all.
		 * A lost window shows up as a code skip and resyncs within <=6 slots. */
		size_t ord = 0;             /* next expected slot */
		int last_hop = -1;          /* continuation append target (multi-buffer windows) */
		auto attribute = [&](rx_buf *bp) {
			if (!(bp->rx_flags & 0x1)) {
				/* no DISCONT: sparse-anchor duty (drop) or a continuation of the last window
				 * (append; identity was fixed by its first buffer) */
				if (last_hop >= 0 && hopdone[last_hop] != 1 && hopseen[last_hop] > 0 &&
						hopseen[last_hop] < static_cast<size_t>(slotC(ord ? ord - 1 : 0)) * 384) {
					size_t k = static_cast<size_t>(last_hop);
					size_t p0 = hopseen[k];
					hopseen[k] += bp->elem_cnt;
					size_t c0 = p0 > skip_smp ? p0 : skip_smp;
					size_t c1 = hopseen[k] < skip_smp + N_FFT ? hopseen[k] : skip_smp + N_FFT;
					if (c1 > c0) {
						memcpy(hopbuf[k].data() + (c0 - skip_smp) * 2,
								reinterpret_cast<const int16_t *>(bp->buf) + (c0 - p0) * 2, (c1 - c0) * 4);
						hophave[k] += c1 - c0;
						if (hophave[k] >= static_cast<size_t>(N_FFT)) { hopdone[k] = 1; done++; st->rx_samples += hophave[k]; fft_push(k); }
					}
				}
				return;
			}
			/* staleness gate: a leftover window from the PREVIOUS pass can emit after the pass
			 * boundary and would consume an ordinal slot (a +1 shift for the whole rest of the
			 * pass). The #29 stamp shift is only +-a-few-window-pitches, so anything stamped
			 * more than ~20 ms from "now" is a ghost, never a live window. */
			int32_t age = static_cast<int32_t>(static_cast<uint32_t>(now_ticks()) - bp->phytimer);
			if (age > static_cast<int32_t>(tick_hz / 50) || age < -static_cast<int32_t>(tick_hz / 50)) { pr.al_late++; return; }
			uint32_t c_obs = bp->elem_cnt / 384;
			/* find the first slot >= ord whose length code matches; skipped slots were lost */
			size_t j = ord;
			while (j < slot_hop.size() && j < ord + 6 && slotC(j) != c_obs) { j++; }
			if (j >= slot_hop.size() || slotC(j) != c_obs) { pr.al_late++; last_hop = -1; return; }   /* unidentifiable: drop */
			if (j != ord) { pr.al_mis += static_cast<int>(j - ord); }   /* prof: lost windows */
			ord = j + 1;
			int hop = slot_hop[j];
			last_hop = hop;
			if (hop < 0) { return; }                                    /* pre-roll / sentinel */
			size_t k = static_cast<size_t>(hop);
			if (hopdone[k] == 1) { return; }
			if (g_sweep_prof && getenv("RFNM_SWEEP_DEBUG")) {
				logts("dbg att slot %zu -> LO %.0f: elem %u stamp_slot_delta %d", j, comp.los[k] / 1e6, bp->elem_cnt,
						static_cast<int32_t>(bp->phytimer - slot_tick[j]) / static_cast<int32_t>(chunk_ticks_w));
			}
			size_t p0 = hopseen[k];
			hopseen[k] += bp->elem_cnt;
			size_t c0 = p0 > skip_smp ? p0 : skip_smp;
			size_t c1 = hopseen[k] < skip_smp + N_FFT ? hopseen[k] : skip_smp + N_FFT;
			if (c1 > c0) {
				memcpy(hopbuf[k].data() + (c0 - skip_smp) * 2,
						reinterpret_cast<const int16_t *>(bp->buf) + (c0 - p0) * 2, (c1 - c0) * 4);
				hophave[k] += c1 - c0;
			}
			if (hophave[k] >= static_cast<size_t>(N_FFT)) {
				hopdone[k] = 1;
				done++;
				st->rx_samples += hophave[k];
				fft_push(k);
			}
		};

		/* pre-roll throwaway window: the FIRST scheduled window after the inter-pass idle
		 * delivers nothing (boundary loss, positional - proven by range-shifted sweeps), so
		 * spend it on a window nobody needs. Registered as a slot: under the stamp shift a
		 * real hop's data can arrive stamped in a later slot, never in the pre-roll's. */
		{
			uint64_t pw = now_ticks() + settle_ticks;
			dev.schedule_rx(pw, slotC(slot_tick.size()) * SWQ_TPC);
			slot_tick.push_back(static_cast<uint32_t>(pw));
			slot_hop.push_back(-1);
		}

		/* ---- the ladder: apply -> schedule at now+settle -> drain-attribute until close ---- */
		auto tp_p1 = std::chrono::steady_clock::now();
		double p1_apply = 0, p1_sched = 0, p1_wait = 0;
		for (size_t k = 0; k < nhops && sweep->mode.load(std::memory_order_relaxed) && !g_stop; k++) {
			auto tk0 = std::chrono::steady_clock::now();
			agc.stage(dev, rx_ch.id, comp, k);   /* hop gain rides the SAME apply as the LO */
			dev.set_rx_channel_freq(rx_ch.id, static_cast<int64_t>(comp.los[k]), false);
			/* deadline retry, 10 ms, TRANSIENT codes only: cmd-busy/transport retries ride out
			 * the anchor bursts; a device REJECT (#32 SXR gap "Tune failed") is deterministic
			 * and retrying it only storms the librfnm error log */
			rfnm_api_failcode ar;
			auto rdl = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
			while ((ar = dev.apply(rx_ch.apply, true, 1000000)) != RFNM_API_OK &&
					(ar == RFNM_API_TIMEOUT || ar == RFNM_API_USB_FAIL) &&
					std::chrono::steady_clock::now() < rdl && !g_stop) {
				pr.al_fb++;                                 /* prof: apply retries */
				std::this_thread::sleep_for(std::chrono::microseconds(300));
			}
			auto tk1 = std::chrono::steady_clock::now();
			p1_apply += prof_ms(tk0, tk1);
			if (ar != RFNM_API_OK) { st->stream_errors++; hopdone[k] = 2; pr.al_mis++; continue; }
			/* the confirm is the "FE programmed" edge: open the window one settle later */
			const uint32_t wc = slotC(slot_tick.size());
			uint64_t wk = now_ticks() + settle_ticks;
			if (dev.schedule_rx(wk, wc * SWQ_TPC) != RFNM_API_OK) {
				wk = now_ticks() + settle_ticks;
				if (dev.schedule_rx(wk, wc * SWQ_TPC) != RFNM_API_OK) {
					st->stream_errors++; hopdone[k] = 2; continue;
				}
			}
			htick[k] = static_cast<uint32_t>(wk);
			slot_tick.push_back(static_cast<uint32_t>(wk));
			slot_hop.push_back(static_cast<int>(k));
			hops_sched++;
			auto tk2 = std::chrono::steady_clock::now();
			p1_sched += prof_ms(tk1, tk2);
			g_rec.cur_freq.store(static_cast<uint64_t>(comp.los[k]), std::memory_order_relaxed);
			/* until THIS window closes (retuning earlier would corrupt its tail), consume and
			 * attribute whatever has been delivered - the drain rides inside the ladder's dead
			 * time instead of being a phase of its own */
			const uint64_t close_tick = wk + wc * chunk_ticks_w;
			for (;;) {
				int64_t rem_ticks = static_cast<int64_t>(close_tick - now_ticks());
				if (rem_ticks <= 0 || g_stop) { break; }
				uint32_t rem_us = static_cast<uint32_t>(rem_ticks * 1000000 / static_cast<int64_t>(tick_hz)) + 1;
				rx_buf *bp = nullptr;
				if (dev.rx_dqbuf(&bp, rx_ch.mask, rem_us > 5000 ? 5000 : rem_us) != RFNM_API_OK) { continue; }
				attribute(bp);
				dev.rx_qbuf(bp);
			}
			p1_wait += prof_ms(tk2, std::chrono::steady_clock::now());
		}
		pr.tune_ms = p1_apply; pr.settle_ms = p1_sched; pr.cap_ms = p1_wait;

		/* sentinel windows: without successors queued behind it, the ladder's tail (last ~4
		 * windows pre-sentinel; the last one with 1-chunk sentinels) delivers nothing - the
		 * walker/VSPA sub-path needs follow-on window mass to push a window's data through.
		 * Full-size throwaways spaced 1 ms, registered as slots: under the stamp shift the
		 * LAST K hops' data arrives stamped in sentinel slots. */
		for (int sn = 0; sn < 6; sn++) {
			uint64_t swk = now_ticks() + settle_ticks + static_cast<uint64_t>(sn) * tick_hz / 1000ull;
			dev.schedule_rx(swk, slotC(slot_tick.size()) * SWQ_TPC);
			slot_tick.push_back(static_cast<uint32_t>(swk));
			slot_hop.push_back(-1);
		}

		/* ---- straggler tail: deliveries lag capture by transit only; idle-exit fast ---- */
		auto tp_p2 = std::chrono::steady_clock::now();
		auto tail_dl = tp_p2 + std::chrono::milliseconds(SWQ_TAIL_MS);
		int dry = 0;
		while (done < hops_sched && !g_stop && std::chrono::steady_clock::now() < tail_dl) {
			rx_buf *bp = nullptr;
			if (dev.rx_dqbuf(&bp, rx_ch.mask, 10000) != RFNM_API_OK) {
				if (++dry >= 2) { break; }   /* two empty 10 ms waits: stragglers are never coming */
				continue;
			}
			dry = 0;
			attribute(bp);
			dev.rx_qbuf(bp);
		}
		pr.drain_ms = prof_ms(tp_p2, std::chrono::steady_clock::now());

		/* ---- FFT barrier, then compose + publish (comp is ours again after the barrier) ---- */
		auto tp_p3 = std::chrono::steady_clock::now();
		{
			std::unique_lock<std::mutex> lk(fq_mtx);
			fq_cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return fft_done == fft_queued; });
		}
		sweep_publish(comp, st);
		agc.pass_log();
		pr.fft_ms = prof_ms(tp_p3, std::chrono::steady_clock::now());
		pr.flush_reads = static_cast<int>(done);       /* prof: hops filled to N_FFT */
		pr.ts_gated = static_cast<int>(nhops - done);  /* prof: hold-filled hops this pass */
		pr.log("sched", nhops, prof_ms(tp_pass, std::chrono::steady_clock::now()));
		if (g_sweep_prof && done < nhops) {
			char miss[256]; int mo = 0;
			for (size_t k = 0; k < nhops && mo < 200; k++) {
				if (hopdone[k] == 1) { continue; }
				mo += snprintf(miss + mo, sizeof(miss) - mo, " %.0f/%zu%s", comp.los[k] / 1e6, hophave[k], hopdone[k] == 2 ? "!" : "");
			}
			logts("sweep-prof[sched]: unfilled (LO/have, !=apply-fail):%s", miss);
		}
	}

	/* stop the FFT worker before comp goes out of scope */
	{
		std::lock_guard<std::mutex> lk(fq_mtx);
		fq_stop = true;
		fq_cv.notify_all();
	}
	fft_thr.join();

	/* ---- tear down the scheduled session; leave RT state for rx_source's rx_stream_create ---- */
	dev.rx_work_stop();
	dev.rx_tdd_stop();
	dev.schedule_reset();
	restore_rt();
}


/* librfnm path enum -> ws path code (0 embed, 1..8 SMA a..h, 9 loopback; -1 = not announceable) */
static int path_enum_to_code(enum rfnm_rf_path p) {
	if (p == RFNM_PATH_EMBED_ANT) { return 0; }
	if (p >= RFNM_PATH_SMA_A && p <= RFNM_PATH_SMA_H) { return static_cast<int>(p) + 1; }
	if (p == RFNM_PATH_LOOPBACK) { return 9; }
	return -1;
}

/* build the announced RX path menu from hwinfo + every RX channel's path_possible */
static void path_menu_enumerate(rfnm::device &dev, uint8_t cur_channel, const std::string &cur_path) {
	const struct rfnm_dev_hwinfo *hw = dev.get_hwinfo();
	std::lock_guard<std::mutex> lk(g_pathmenu.mtx);
	g_pathmenu.entries.clear();
	for (int d = 0; d < 2; d++) {
		snprintf(g_pathmenu.dgb_name[d], sizeof(g_pathmenu.dgb_name[d]), "%.29s", hw ? hw->daughterboard[d].user_readable_name : "");
	}
	uint32_t nch = dev.get_rx_channel_count();
	for (uint32_t ch = 0; ch < nch && ch < 8; ch++) {
		const struct rfnm_api_rx_ch *rc = dev.get_rx_channel(ch);
		if (!rc) { continue; }
		uint8_t dgb = static_cast<uint8_t>(rc->dgb_id == 1 ? 1 : 0);
		/* the channel's tunable range, MHz, rounded inward; clients clamp their zoom/sweep
		 * world to the SELECTED path's range (fallback to the platform 10..3500 if unset) */
		uint16_t fmin = 10, fmax = 3500;
		if (rc->freq_max > rc->freq_min && rc->freq_max > 0) {
			fmin = static_cast<uint16_t>((rc->freq_min + 999999) / 1000000);
			fmax = static_cast<uint16_t>(rc->freq_max / 1000000);
			if (fmin < 1) { fmin = 1; }
		}
		for (int i = 0; i < 10; i++) {
			int code = path_enum_to_code(rc->path_possible[i]);
			if (code < 0) { continue; }
			/* one entry per PHYSICAL connector: dedupe on (daughterboard, path) — two channels
			 * of the same board exposing SMA A are lanes, not user choices; the lowest channel
			 * takes it (yucca showed "SMA A" twice in the 07-12 verify pass) */
			bool dup = false;
			for (auto &e : g_pathmenu.entries) { if (e.code == code && e.dgb == dgb) { dup = true; break; } }
			if (!dup && g_pathmenu.entries.size() < RFSP_PATHS_MAX) {
				g_pathmenu.entries.push_back({static_cast<uint8_t>(code), static_cast<uint8_t>(ch), dgb, fmin, fmax});
			}
		}
	}
	int cur_code = 0;
	for (int c = 0; c <= 9; c++) { if (cur_path == path_code_str(c)) { cur_code = c; break; } }
	g_pathmenu.cur_code = static_cast<uint8_t>(cur_code);
	g_pathmenu.cur_channel = cur_channel;
	g_pathmenu.gen.fetch_add(1, std::memory_order_release);
}

/* record a path/channel change so the ws thread re-announces the menu */
static void path_menu_set_current(uint8_t channel, const std::string &path) {
	std::lock_guard<std::mutex> lk(g_pathmenu.mtx);
	int cur_code = 0;
	for (int c = 0; c <= 9; c++) { if (path == path_code_str(c)) { cur_code = c; break; } }
	g_pathmenu.cur_code = static_cast<uint8_t>(cur_code);
	g_pathmenu.cur_channel = channel;
	g_pathmenu.gen.fetch_add(1, std::memory_order_release);
}

static void rx_source(Cfg *cfg, TuneBox *tune, SweepBox *sweep, SlotPool *sp, RowRing *rr, Stats *st) {
	using namespace rfnm;
	pin_self(3);
	try {
		device dev(TRANSPORT_LOCAL, "");
		rfnm_api_failcode ret;
		if ((ret = dev.set_stream_format(STREAM_FORMAT_CS16))) { logts("stream format fail %d", ret); g_stop = 1; return; }
		if ((ret = dev.set_samp_rate(static_cast<uint64_t>(cfg->rate), 1000000))) { logts("samp rate fail %d", ret); g_stop = 1; return; }
		auto rx_ch = dev.rx_ch_helper(0);
		dev.set_rx_channel_freq(rx_ch.id, static_cast<uint64_t>(cfg->freq), false);
		int agc_gmin = cfg->gain, agc_gmax = cfg->gain;
		{
			/* clamp the reference gain to the device's advertised range (an out-of-range
			 * hardcode is an apply reject); the range also bounds the leveling steppers */
			const struct rfnm_api_rx_ch *rc = dev.get_rx_channel(rx_ch.id);
			if (rc && static_cast<int>(rc->gain_range.max) > static_cast<int>(rc->gain_range.min)) {
				agc_gmin = rc->gain_range.min;
				agc_gmax = rc->gain_range.max;
				if (cfg->gain < agc_gmin) { cfg->gain = agc_gmin; }
				if (cfg->gain > agc_gmax) { cfg->gain = agc_gmax; }
			}
		}
		dev.set_rx_channel_gain(rx_ch.id, cfg->gain, false);
		/* the device agc field stays OFF for every spectrumd session: leveling is host-side
		 * (see Cfg::agc) and the kernel loop must never own gain under a local client */
		dev.set_rx_channel_agc(rx_ch.id, RFNM_AGC_OFF, false);
		g_agc_on.store(cfg->agc ? 1 : 0, std::memory_order_relaxed);
		g_agc_gain.store(cfg->gain, std::memory_order_relaxed);
		if (!cfg->path.empty()) {
			enum rfnm_rf_path pth = device::string_to_rf_path(cfg->path);
			dev.set_rx_channel_path(rx_ch.id, pth, false);
			logts("RF path: %s", device::rf_path_to_string(pth).c_str());
		}
		{
			/* initialize the RFIC LPF to the sample rate: boards were inheriting leftover
			 * RFIC state (Lime booted with a narrow LPF -> only the DC skirt visible) */
			dev.set_rx_channel_rfic_lpf_bw(rx_ch.id, rfic_lpf_for(cfg->rate), false);
		}
		/* #39: an RTSA is a panorama instrument - hold ONE broadband FE leg per region so
		 * hop/retune levels stay comparable (the banked GNSS/cellular SAW legs read as
		 * fake noise-floor shelves wherever a sweep ladder or retune walks past them).
		 * RFNM_FE_MODE=auto keeps the device's capture-fit default for A/B experiments. */
		const enum rfnm_rx_fe_mode fe_mode =
				(getenv("RFNM_FE_MODE") && strcmp(getenv("RFNM_FE_MODE"), "auto") == 0) ? RFNM_RX_FE_AUTO : RFNM_RX_FE_FLAT;
		dev.set_rx_channel_fe_mode(rx_ch.id, fe_mode, false);
		dev.set_rx_channel_status(rx_ch.id, RFNM_CH_RF_OFF, RFNM_CH_STREAM_AUTO, false);
		if ((ret = dev.apply(rx_ch.apply, true, 1000000))) { logts("apply fail %d", ret); g_stop = 1; return; }
		std::unique_ptr<rx_stream> stream(dev.rx_stream_create(rx_ch.mask));
		stream->set_auto_dc_offset(false);
		if ((ret = stream->start())) { logts("stream start fail %d", ret); g_stop = 1; return; }
		logts("local RX up: %.3f MHz LO, %.2f MSPS, gain %d, agc %s", cfg->freq / 1e6, cfg->rate / 1e6, cfg->gain, cfg->agc ? "on" : "off");
		uint8_t rx_channel = 0;   /* RX channel the session drives (antenna picks can move it) */
		if (cfg->path.empty()) {
			/* no --path given: adopt the channel's actual default so the announced menu is truthful */
			const struct rfnm_api_rx_ch *rc0 = dev.get_rx_channel(rx_ch.id);
			int c0 = rc0 ? path_enum_to_code(rc0->path) : -1;
			cfg->path = c0 >= 0 ? path_code_str(c0) : "embed";
		}
		path_menu_enumerate(dev, rx_channel, cfg->path);
		{
			/* restore the user's antenna across restarts (watchdog/appd cycles reset the unit
			 * default otherwise = "it changed my antenna by itself"). Injected via the normal
			 * switch mailbox so the full clamp/rebuild machinery handles it on loop entry.
			 * An explicit --path terminated session (dead input, the flatness rig) is a
			 * deliberate test state: never auto-restore an antenna over it. */
			uint8_t pc_ch = 0;
			char pc_path[24] = {0};
			if (cfg->path != "terminated" &&
					path_persist_load(&pc_ch, pc_path, sizeof(pc_path)) && (pc_ch != rx_channel || cfg->path != pc_path)) {
				for (int c = 0; c <= 9; c++) {
					if (strcmp(pc_path, path_code_str(c)) == 0) {
						tune->path_req.store(static_cast<int32_t>(pc_ch) << 8 | c, std::memory_order_release);
						logts("restoring persisted antenna: ch%u %s", pc_ch, pc_path);
						break;
					}
				}
			}
		}
		/* publish tuning for the recorder (record-what-you-see) */
		g_rec.cur_freq.store(static_cast<uint64_t>(cfg->freq), std::memory_order_relaxed);
		g_rec.cur_rate.store(static_cast<uint32_t>(cfg->rate), std::memory_order_relaxed);
		g_rec.cur_gain.store(cfg->gain, std::memory_order_relaxed);
		snprintf(g_rec.cur_path, sizeof(g_rec.cur_path), "%s", cfg->path.c_str());

		std::vector<int16_t> chunk(1 << 20);       /* 512k samples = 128 windows */
		void *bufs[1] = {chunk.data()};
		size_t elems;
		uint64_t ts;
		Slot *cur = nullptr;
		int row_fill = 0;
		for (int i = 0; i < 8 && !g_stop; i++) { stream->read(bufs, chunk.size() / 2, elems, ts, 100000); }
		bool parked = false;                 /* idle auto-park: RX stream released when no WS client */
		uint64_t idle_since = 0;             /* ms timestamp when clients hit 0 (0 = not idle) */
		auto now_ms = []() -> uint64_t {
			return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		};
		/* RT leveling stepper (host-side twin of the sweep table, one "hop"): scan a slice of
		 * every read, evaluate a two-rate step per 50 ms window, actuate through the normal
		 * gain apply. Same detector as SweepAgc::update — peak-off-mean on the I lane only
		 * (see the rationale there: DC immunity + the Lime Q-lane rail under a frozen DC
		 * null). Blanked 300 ms across every discontinuity (retune, antenna, span step,
		 * park-resume, sweep exit) — never step across one. */
		int rt_gain = cfg->gain;
		int64_t rt_si = 0, rt_sq = 0;
		int64_t rt_n = 0;
		int rt_min_i = 32767, rt_max_i = -32768;
		int rt_min_q = 32767, rt_max_q = -32768;
		uint64_t rt_win_start = 0, rt_blank_until = 0, rt_below_since = 0;
		auto rt_win_reset = [&]() {
			rt_si = 0; rt_sq = 0; rt_n = 0;
			rt_min_i = 32767; rt_max_i = -32768;
			rt_min_q = 32767; rt_max_q = -32768;
		};
		auto rt_agc_blank = [&](uint64_t ms) {
			rt_blank_until = now_ms() + ms;
			rt_win_start = 0;
			rt_below_since = 0;
			rt_win_reset();
		};
		/* client-door dc-null state (lambdas below; declared here because the leveling stepper
		 * yields to the null while it is armed) */
		int dcn_ti = 0, dcn_tq = 0;       /* codes we believe stand on the chip (a kernel-null trim
		                                   * from a prior native session may differ - the measured
		                                   * loop converges on chip truth regardless) */
		int dcn_t0i = 0, dcn_t0q = 0;     /* codes at arm time: restored if a give-up left it worse */
		int dcn_m0 = 0;                   /* dominant |mean| at the first armed measurement */
		int dcn_iters = 0;
		bool dcn_armed = false, dcn_map_crossed = true;   /* delivered-Q <- rfic_dc_i (local fill); armed via dcn_arm below */
		int64_t dcn_si = 0, dcn_sq = 0, dcn_n = 0;
		uint64_t dcn_win_start = 0;
		int dcn_prev_mi = 0, dcn_prev_mq = 0, dcn_prev_step = 0;
		double dcn_slope = 0;             /* learned |stream counts per code| on the delivered lanes */
		auto rt_agc_step = [&](const int16_t *iq, size_t elems) {
			if (!cfg->agc || !elems) { return; }
			if (dcn_armed) { return; }   /* the dc-null owns the stream until it disarms: leveling
			                              * on un-nulled lanes is noise, and a gain step between
			                              * two null measurements poisons its slope/map learning
			                              * (first-start receipts 2026-07-21: map mis-flip + trim
			                              * slammed to (63,63) by exactly that interleave) */
			uint64_t nowa = now_ms();
			if (nowa < rt_blank_until) { return; }
			size_t nv = elems * 2;
			if (nv > 16384) { nv = 16384; }   /* slice per read: ~0.8M vals/s of coverage, negligible cost */
			for (size_t i = 0; i + 1 < nv; i += 2) {
				int vi = iq[i];
				int vq = iq[i + 1];
				if (vi == -32768 || vq == -32768) { continue; }   /* 0x8000 = fill/park sentinel, never ADC data */
				rt_si += vi;
				rt_sq += vq;                      /* Q mean: step log + the dc-null shares the domain */
				rt_n++;
				if (vi < rt_min_i) { rt_min_i = vi; }
				if (vi > rt_max_i) { rt_max_i = vi; }
				if (g_iq_det) {
					if (vq < rt_min_q) { rt_min_q = vq; }
					if (vq > rt_max_q) { rt_max_q = vq; }
				}
			}
			if (!rt_win_start) { rt_win_start = nowa; return; }
			if (nowa - rt_win_start < 50 || rt_n < 256) { return; }
			int mi = static_cast<int>(rt_si / rt_n), mq = static_cast<int>(rt_sq / rt_n);
			int ac = rt_max_i - mi;
			if (mi - rt_min_i > ac) { ac = mi - rt_min_i; }
			int raw = rt_max_i > -rt_min_i ? rt_max_i : -rt_min_i;
			if (g_iq_det && rt_max_q > rt_min_q) {
				int acq = rt_max_q - mq;
				if (mq - rt_min_q > acq) { acq = mq - rt_min_q; }
				if (acq > ac) { ac = acq; }
				int rawq = rt_max_q > -rt_min_q ? rt_max_q : -rt_min_q;
				if (rawq > raw) { raw = rawq; }
			}
			int g = rt_gain;
			rt_win_reset();
			rt_win_start = nowa;
			if (ac >= SWEEP_AGC_ATT) {
				int down = 6;
				if (ac >= SWEEP_AGC_PIN) { down = 12; }
				else { int o = ac / SWEEP_AGC_ATT; while (o > 1) { down += 6; o >>= 1; } }
				g -= down;
				if (g < agc_gmin) { g = agc_gmin; }
				rt_below_since = 0;
			} else if (ac && ac < SWEEP_AGC_DEC && raw < SWEEP_AGC_ATT) {
				if (!rt_below_since) { rt_below_since = nowa; return; }
				if (nowa - rt_below_since < 500) { return; }
				rt_below_since = 0;
				if (g < agc_gmax) { g += SWEEP_AGC_UP_DB; if (g > agc_gmax) { g = agc_gmax; } }
			} else {
				rt_below_since = 0;
				return;
			}
			if (g == rt_gain) { return; }
			dev.set_rx_channel_gain(rx_ch.id, static_cast<int8_t>(g), false);
			if (dev.apply(rx_ch.apply, true, 1000000) == RFNM_API_OK) {
				logts("rt-agc: ac=%d raw=%d dc=(%d,%d) gain %d -> %d", ac, raw, mi, mq, rt_gain, g);
				rt_gain = g;
				g_agc_gain.store(g, std::memory_order_relaxed);
				g_rec.cur_gain.store(g, std::memory_order_relaxed);
			}
			rt_agc_blank(50);   /* settle after our own step (or a failed apply) */
		};
		/* One-shot client-door analog DC null (2026-07-21 reform). Local sessions run DCS/decim
		 * clock plans, so the kernel null in rfnm_qec is format-gated OFF here - the CLIENT owns
		 * the correction, through rfic_dc_i/q: the ONE public RFIC IQ correction (logical codes
		 * +-126 spanning the full silicon authority; how the board fans them over its internal
		 * knobs is driver business, invisible here).
		 * Armed at session start and every analog discontinuity; measures the lane means off the
		 * live stream, writes the LMS DCOFF codes (+-63) in the same confirmed apply the gain
		 * steps ride, then refines with the MEASURED slope - the counts/code plant varies with
		 * LO, PGA and clock plan (9.4 at 61.44M vs ~4 at 122.88M, P0 receipts), so assumptions
		 * only seed the first step. The delivered-lane <-> chip-knob mapping is auto-learned
		 * (the local CS16 fill and the USB path cross the lanes differently): if the targeted
		 * lane refuses to move, flip the map and keep iterating. The rfnm_lime DC guard caps
		 * the PGA until this trim lands, so pre-null windows are un-railed by construction;
		 * the trim write also stamps guard freshness at the current LO, restoring full PGA. */
		auto dcn_arm = [&]() {
			if (!g_dc_null) { return; }
			/* seed the mirror from chip truth: the kernel echoes the last-applied rfic_dc
			 * in GET_RX_CH_LIST, so the lib struct holds the standing trim even across a
			 * process restart. Without this a fresh session's (0,0) mirror CLOBBERS a good
			 * standing trim with its first "small" absolute write, the lanes blow back up,
			 * and the map-learn misreads the self-inflicted damage as a refused move and
			 * flips to the wrong map (2026-07-21 service-start receipt). */
			if (const struct rfnm_api_rx_ch *c0 = dev.get_rx_channel(rx_ch.id)) {
				dcn_ti = c0->rfic_dc_i;
				dcn_tq = c0->rfic_dc_q;
			}
			dcn_armed = true;
			dcn_iters = 0;
			dcn_slope = 0;
			dcn_prev_step = 0;
			dcn_t0i = dcn_ti;
			dcn_t0q = dcn_tq;
			dcn_m0 = 0;
			dcn_si = 0; dcn_sq = 0; dcn_n = 0; dcn_win_start = 0;
		};
		auto rt_dc_null = [&](const int16_t *iq, size_t n_elems) {
			if (!dcn_armed || !n_elems) { return; }
			uint64_t nowa = now_ms();
			if (nowa < rt_blank_until) { return; }   /* share the leveling blanks: never measure a transient */
			size_t nv = n_elems * 2;
			for (size_t i = 0; i + 1 < nv; i += 2) {
				int vi = iq[i], vq = iq[i + 1];
				if (vi == -32768 || vq == -32768) { continue; }   /* fill/park sentinel pair */
				dcn_si += vi;
				dcn_sq += vq;
				dcn_n++;
			}
			if (!dcn_win_start) { dcn_win_start = nowa; return; }
			if (nowa - dcn_win_start < 80 || dcn_n < 100000) { return; }
			int mi = static_cast<int>(dcn_si / dcn_n), mq = static_cast<int>(dcn_sq / dcn_n);
			dcn_si = 0; dcn_sq = 0; dcn_n = 0; dcn_win_start = nowa;
			int mdom = std::abs(mi) > std::abs(mq) ? std::abs(mi) : std::abs(mq);
			if (!dcn_m0) { dcn_m0 = mdom; }
			/* give-up path: if we made things WORSE than the arm point (poisoned learn, plant
			 * surprise), put the arm-time codes back - never leave a garbage trim standing.
			 * (0,0) cannot be commanded through this door (the driver skips the all-zero
			 * write), so a zero start restores to (1,1) = one code (~9 counts) off zero. */
			auto dcn_finish = [&](const char *why) {
				if (mdom > dcn_m0 && (dcn_ti != dcn_t0i || dcn_tq != dcn_t0q)) {
					int ri = dcn_t0i, rq = dcn_t0q;
					if (!ri && !rq) { ri = 1; rq = 1; }
					auto *chr = const_cast<struct rfnm_api_rx_ch *>(dev.get_rx_channel(rx_ch.id));
					if (chr) {
						chr->rfic_dc_i = static_cast<int16_t>(ri);
						chr->rfic_dc_q = static_cast<int16_t>(rq);
						if (dev.apply(rx_ch.apply, true, 1000000) == RFNM_API_OK) {
							dcn_ti = ri;
							dcn_tq = rq;
						}
					}
					logts("dc-null: %s WORSE than start (%d > %d) - restored trim (%d,%d)", why, mdom, dcn_m0, dcn_ti, dcn_tq);
				} else {
					logts("dc-null: %s, mean=(%d,%d) trim=(%d,%d)", why, mi, mq, dcn_ti, dcn_tq);
				}
				dcn_armed = false;
			};
			if (mi < 512 && mi > -512 && mq < 512 && mq > -512) {   /* <=32 12-bit counts per lane: done */
				if (dcn_iters) { logts("dc-null: converged, mean=(%d,%d) trim=(%d,%d) after %d steps", mi, mq, dcn_ti, dcn_tq, dcn_iters); }
				dcn_armed = false;
				return;
			}
			if (dcn_iters >= 6) {
				dcn_finish((dcn_ti >= 126 || dcn_ti <= -126 || dcn_tq >= 126 || dcn_tq <= -126) ?
						"iter cap (authority edge)" : "iter cap");
				return;
			}
			if (dcn_prev_step) {
				/* slope learn from the last step, on the lane it targeted. Leveling yields while
				 * we are armed, so the only apply between two measurements was OURS - the delta
				 * is attributable. A CLEARLY opposite move on the targeted lane (not noise)
				 * means the lane<->knob map is crossed the other way: flip once and re-learn. */
				int moved = dcn_prev_mi - mi;
				if (std::abs(dcn_prev_mq - mq) > std::abs(moved)) { moved = dcn_prev_mq - mq; }
				double s = static_cast<double>(moved) / dcn_prev_step;
				if (s > 8.0) { dcn_slope = s; }
				else if (dcn_iters == 1 && s < -8.0) { dcn_map_crossed = !dcn_map_crossed; dcn_slope = 0; }
			}
			double slope = dcn_slope;
			if (slope <= 0) {
				/* seed only: ~9 counts/code (<<4 stream) - the board's internal gain split
				 * scales the true slope (not exposed, by API design) and the learn above
				 * replaces the seed after the first step; a seed understep costs one
				 * iteration, nothing else */
				slope = 9.0 * 16.0;
			}
			/* crossed map: rfic_dc_i serves delivered-Q and vice versa (local CS16 fill);
			 * straight map: knob i <-> delivered I. Positive mean -> positive code, the same
			 * convention the kernel null uses through the same board-crossing-corrected door. */
			int step_i_knob = static_cast<int>(std::lround((dcn_map_crossed ? mq : mi) / slope));
			int step_q_knob = static_cast<int>(std::lround((dcn_map_crossed ? mi : mq) / slope));
			if (!step_i_knob && !step_q_knob) {
				if (std::abs(dcn_map_crossed ? mq : mi) >= std::abs(dcn_map_crossed ? mi : mq)) {
					step_i_knob = (dcn_map_crossed ? mq : mi) > 0 ? 1 : -1;
				} else {
					step_q_knob = (dcn_map_crossed ? mi : mq) > 0 ? 1 : -1;
				}
			}
			int ti = dcn_ti + step_i_knob;
			int tq = dcn_tq + step_q_knob;
			if (ti > 126) { ti = 126; } if (ti < -126) { ti = -126; }
			if (tq > 126) { tq = 126; } if (tq < -126) { tq = -126; }
			if (ti == dcn_ti && tq == dcn_tq) {
				dcn_finish("authority edge");
				return;
			}
			/* client-owned manual trim fields (no lib setter): poke the live struct, ride an apply */
			auto *chs = const_cast<struct rfnm_api_rx_ch *>(dev.get_rx_channel(rx_ch.id));
			if (!chs) { dcn_armed = false; return; }
			chs->rfic_dc_i = static_cast<int16_t>(ti);
			chs->rfic_dc_q = static_cast<int16_t>(tq);
			if (dev.apply(rx_ch.apply, true, 1000000) != RFNM_API_OK) {
				logts("dc-null: trim apply failed, freezing");
				dcn_armed = false;
				return;
			}
			dcn_prev_mi = mi;
			dcn_prev_mq = mq;
			dcn_prev_step = std::abs(ti - dcn_ti) >= std::abs(tq - dcn_tq) ? ti - dcn_ti : tq - dcn_tq;
			dcn_ti = ti;
			dcn_tq = tq;
			dcn_iters++;
			logts("dc-null: iter %d mean=(%d,%d) trim=(%d,%d)%s", dcn_iters, mi, mq, ti, tq, dcn_map_crossed ? "" : " (map straight)");
			rt_blank_until = now_ms() + 80;   /* let the trim transient pass; leveling shares the blank */
		};
		dcn_arm();   /* session start: first null, mirror seeded from the standing chip trim */
		while (!g_stop) {
			/* Idle auto-park: with no WS client connected, stop the RX stream so the radio and
			 * its RX DMA kthread go truly idle. The old clientless path still ran stream->read
			 * (drain-only), pinning the RX kthread at the full rate. A few seconds' grace avoids
			 * thrashing on tab switches; a sweep or an active recording implies a client, so we
			 * never park during those. Uses the same stop/reset/recreate the sweep path uses. */
			{
				int nc = st->clients.load(std::memory_order_relaxed);
				if (parked) {
					if (nc == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(150)); continue; }
					stream.reset(dev.rx_stream_create(rx_ch.mask));   /* client returned: rebuild RT stream */
					stream->set_auto_dc_offset(false);
					if (stream->start() != RFNM_API_OK) {
						std::this_thread::sleep_for(std::chrono::milliseconds(200));
						if (stream->start() != RFNM_API_OK) { logts("resume: RX stream start failed"); g_stop = 1; return; }
					}
					for (int i = 0; i < 8 && !g_stop; i++) { stream->read(bufs, chunk.size() / 2, elems, ts, 100000); }
					parked = false; idle_since = 0; cur = nullptr; row_fill = 0;
					rt_agc_blank(300);   /* stream restart: fresh leveling window */
					dcn_arm();           /* re-null on resume: LO/temp may have walked while parked */
					st->rx_parked.store(false, std::memory_order_relaxed);
					logts("WS client connected - RX resumed");
				} else if (nc == 0 && !sweep->mode.load(std::memory_order_relaxed) && !g_rec.active.load(std::memory_order_relaxed)) {
					uint64_t np = now_ms();
					if (idle_since == 0) { idle_since = np; }
					else if (np - idle_since >= RX_PARK_GRACE_MS) {
						stream->stop(); stream.reset(); parked = true;
						st->rx_parked.store(true, std::memory_order_relaxed);
						logts("no WS client for %us - RX parked (radio idle)", (unsigned)(RX_PARK_GRACE_MS / 1000));
						continue;
					}
				} else {
					idle_since = 0;
				}
			}
			/* swept mode: pick the acquisition engine by per-hop bandwidth. At >= SWEEP_SCHED_MIN_BW
			 * the schedule_rx engine is exact + fast (one delivered window buffer already exceeds
			 * N_FFT, so the librfnm reorder feeds it cleanly). Below that the fat scheduled windows
			 * fragment into many small buffers and the reorder stalls on the gaps, so fall back to
			 * the drain engine (robust at every rate, just slower). The schedule_rx engine owns a
			 * PRIVATE scheduled session (its own anchor + rx_work loop) that can't coexist with the
			 * RT rx_stream's work loop, so it needs the stream torn down/rebuilt; the drain engine
			 * reuses the live rx_stream. Both restore the entry rate/LO before returning. */
			if (sweep->mode.load(std::memory_order_relaxed)) {
				/* DRAIN is the default at every bandwidth: it reuses the live rx_stream and its
				 * levels are RT-exact (verified +0.17 dB mean vs an RT ladder). The schedule_rx
				 * engine is faster but the fw cannot yet bind window data to its request
				 * (residual: the VSPA stamp queue drifts per session AND piecewise
				 * within a pass, displacing whole hops - stamps, ordinals and length codes all
				 * measured unreliable); opt-in via RFNM_SWEEP_SCHED=1 for fw work
				 * until the per-window handshake lands. */
				if (sweep->bw_hz.load() >= SWEEP_SCHED_MIN_BW && getenv("RFNM_SWEEP_SCHED")) {
					g_agc_gain.store(cfg->gain, std::memory_order_relaxed);   /* STATUS shows the ref during sweep */
					stream->stop();
					stream.reset();                                  /* release the RT work loop + pool */
					run_sweep_sched(dev, rx_ch, cfg, sweep, st);
					/* sweep -> RT edge: restore the reference gain (the sched teardown left the
					 * last hop's table gain applied) before the stream comes back */
					dev.set_rx_channel_gain(rx_ch.id, static_cast<int8_t>(cfg->gain), false);
					dev.apply(rx_ch.apply, true, 1000000);
					stream.reset(dev.rx_stream_create(rx_ch.mask));  /* rebuild the RT stream */
					stream->set_auto_dc_offset(false);
					/* patient rebuild: the first start after a scheduled session fails transiently
					 * (RF_ON re-own + reorder re-adopt); killing the daemon here (old g_stop=1) took
					 * the whole GUI down on EVERY sweep exit - the morph's "black gap" (browser pass
					 * 07-12). Retry with fresh stream objects; only a persistent failure exits. */
					{
						int tries = 5;
						while (stream->start() != RFNM_API_OK && --tries && !g_stop) {
							std::this_thread::sleep_for(std::chrono::milliseconds(300));
							stream.reset(dev.rx_stream_create(rx_ch.mask));
							stream->set_auto_dc_offset(false);
						}
						if (!tries) { logts("sweep exit: RT stream restart failed after retries"); g_stop = 1; return; }
					}
					for (int i = 0; i < 8 && !g_stop; i++) { stream->read(bufs, chunk.size() / 2, elems, ts, 100000); }
				} else {
					/* RT state is saved ONCE at the RT -> sweep edge; gen re-arms (the zoom-following
					 * sweep range) loop here with no restore and, with the bw unchanged, no reclock —
					 * a range change is just a new hop ladder, live within one pass. */
					double rt_rate = cfg->rate, rt_freq = cfg->freq;
					g_agc_gain.store(cfg->gain, std::memory_order_relaxed);   /* STATUS shows the ref during sweep */
					while (sweep->mode.load(std::memory_order_relaxed) && !g_stop) {
						run_sweep_drain(dev, stream, rx_ch, cfg, sweep, st);
					}
					/* sweep -> RT edge: a morphing client sends TUNE/RATE right behind the sweep
					 * disable — fold those into the restore so the morph costs ONE reclock, not a
					 * restore-then-step double. */
					uint64_t mf = tune->freq_mhz1000.exchange(0, std::memory_order_acq_rel);
					uint32_t mr = tune->rate_req.exchange(0, std::memory_order_acq_rel);
					double to_rate = mr ? static_cast<double>(mr) : rt_rate;
					double to_freq = mf ? static_cast<double>(mf) * 1000.0 : rt_freq;
					/* restore the reference gain (the ladder left the last hop's table gain
					 * applied); staged here so the ONE restore apply below carries it */
					dev.set_rx_channel_gain(rx_ch.id, static_cast<int8_t>(cfg->gain), false);
					if (cfg->rate != to_rate) {
						/* discipline mirrors the sweep-entry set_rate: stop -> reclock (retried) ->
						 * stage lpf+freq -> APPLY -> start with recreated stream objects. The first
						 * cut started an un-applied delta on the same stream object and died after
						 * 5 failed retries (the 17:46:14 daemon exit). */
						stream->stop();
						rfnm_api_failcode rr = RFNM_API_TIMEOUT;
						for (int i = 0; i < 4 && !g_stop; i++) {
							rr = dev.set_samp_rate(static_cast<uint64_t>(to_rate), 20000000);
							if (rr == RFNM_API_OK) { break; }
							logts("sweep exit: set_samp_rate %.0fM failed (%d), retry %d", to_rate / 1e6, rr, i + 1);
							std::this_thread::sleep_for(std::chrono::milliseconds(200));
						}
						if (rr == RFNM_API_OK) {
							cfg->rate = to_rate;
							dev.set_rx_channel_rfic_lpf_bw(rx_ch.id, rfic_lpf_for(to_rate), false);
						}
						dev.set_rx_channel_freq(rx_ch.id, static_cast<int64_t>(to_freq), false);
						if (dev.apply(rx_ch.apply, true, 5000000) == RFNM_API_OK) { cfg->freq = to_freq; }
						int tries = 5;
						while (stream->start() != RFNM_API_OK && --tries && !g_stop) {
							std::this_thread::sleep_for(std::chrono::milliseconds(300));
							stream.reset(dev.rx_stream_create(rx_ch.mask));
							stream->set_auto_dc_offset(false);
						}
						if (!tries) { logts("sweep exit: RT stream restart failed after retries"); g_stop = 1; return; }
						for (int i = 0; i < 8 && !g_stop; i++) { stream->read(bufs, chunk.size() / 2, elems, ts, 100000); }
					} else {
						dev.set_rx_channel_freq(rx_ch.id, static_cast<int64_t>(to_freq), false);
						if (dev.apply(rx_ch.apply, true, 1000000) == RFNM_API_OK) { cfg->freq = to_freq; }
					}
					g_rec.cur_freq.store(static_cast<uint64_t>(cfg->freq), std::memory_order_relaxed);
					g_rec.cur_rate.store(static_cast<uint32_t>(cfg->rate), std::memory_order_relaxed);
					logts("sweep exit: RT restored %.3f MHz @ %.2f MSPS", cfg->freq / 1e6, cfg->rate / 1e6);
				}
				rt_gain = cfg->gain;   /* the sweep table owned gain: reseed the RT stepper at the reference */
				rt_agc_blank(300);
				dcn_arm();               /* sweep exit: the ladder moved LO/gain under us */
				cur = nullptr; row_fill = 0;   /* discard any half-filled RT slot from before (holds no ticket) */
				continue;
			}
			/* live retune requested? (ws thread mailbox) */
			uint64_t fkhz = tune->freq_mhz1000.exchange(0, std::memory_order_acq_rel);
			int32_t g = tune->gain.exchange(INT32_MIN, std::memory_order_acq_rel);
			int32_t areq = tune->agc_req.exchange(-1, std::memory_order_acq_rel);
			if (g != INT32_MIN) { areq = 0; }   /* an explicit gain IS the manual override */
			if (fkhz || g != INT32_MIN) {
				if (fkhz) { dev.set_rx_channel_freq(rx_ch.id, fkhz * 1000ull, false); }
				if (g != INT32_MIN) { dev.set_rx_channel_gain(rx_ch.id, static_cast<int8_t>(g), false); }
				rfnm_api_failcode tret = dev.apply(rx_ch.apply, true, 1000000);
				if (tret == RFNM_API_OK) {
					if (fkhz) { cfg->freq = static_cast<double>(fkhz) * 1000.0; g_rec.cur_freq.store(static_cast<uint64_t>(cfg->freq), std::memory_order_relaxed); }
					if (g != INT32_MIN) {
						cfg->gain = g;
						rt_gain = g;
						g_rec.cur_gain.store(cfg->gain, std::memory_order_relaxed);
						g_agc_gain.store(cfg->gain, std::memory_order_relaxed);
					}
					logts("retuned: %.3f MHz gain %d", cfg->freq / 1e6, cfg->gain);
				} else {
					logts("retune failed (%d)", tret);
				}
				rt_agc_blank(300);   /* content just changed under the stepper either way */
				dcn_arm();           /* retune: the analog DC is LO-dependent (P0 map) */
			}
			if (areq >= 0 && (areq != 0) != cfg->agc) {
				if (!areq) {
					cfg->gain = rt_gain;   /* AGC off = freeze at the stepper's gain (it IS applied) */
				} else {
					rt_agc_blank(300);     /* resume leveling from the current gain, fresh window */
				}
				cfg->agc = areq != 0;
				g_agc_on.store(cfg->agc ? 1 : 0, std::memory_order_relaxed);
				logts("agc %s (gain %d)", cfg->agc ? "on" : "off", cfg->gain);
			}
			/* live antenna / RF-path switch (ws mailbox). Same-channel picks apply in place like a
			 * freq/gain retune — NO stream stop/start: that ~250ms gap starved the WS sender,
			 * dropping the client, whose reconnect re-sent the path, quiesced again -> a
			 * crash-looking reconnect spiral. Cross-channel picks (a path on the OTHER
			 * daughterboard) move the whole RX session: stream teardown, channel swap, rebuild.
			 * Skip entirely when nothing changes. */
			int32_t preq = tune->path_req.exchange(-1, std::memory_order_acq_rel);
			if (preq >= 0) {
				uint8_t pch = static_cast<uint8_t>((preq >> 8) & 0xff);
				const char *pstr = path_code_str(preq & 0xff);
				if (pstr[0] && pch == rx_channel && cfg->path != pstr) {
					enum rfnm_rf_path pth = device::string_to_rf_path(pstr);
					rt_agc_blank(300);   /* new antenna = new levels; never step across the switch */
					dcn_arm();           /* path change moves the FE leg: re-measure the DC */
					dev.set_rx_channel_path(rx_ch.id, pth, false);
					rfnm_api_failcode pret = dev.apply(rx_ch.apply, true, 1000000);
					if (pret == RFNM_API_OK) {
						cfg->path = pstr;
						snprintf(g_rec.cur_path, sizeof(g_rec.cur_path), "%s", pstr);
						path_menu_set_current(rx_channel, cfg->path);
						path_persist_save(rx_channel, pstr);
						logts("antenna -> %s", device::rf_path_to_string(pth).c_str());
					} else {
						path_menu_set_current(rx_channel, cfg->path);   /* re-announce: clients snap back */
						logts("antenna switch failed (%d)", pret);
					}
				} else if (pstr[0] && pch != rx_channel && pch < dev.get_rx_channel_count()) {
					enum rfnm_rf_path pth = device::string_to_rf_path(pstr);
					stream->stop();
					dev.set_rx_channel_status(rx_ch.id, RFNM_CH_RF_OFF, RFNM_CH_STREAM_OFF, false);
					dev.apply(rx_ch.apply, true, 1000000);
					ch_helper nch = dev.rx_ch_helper(pch);
					/* the target channel's band may exclude the current LO (yucca 3300-3800 vs a
					 * 1475 view = the "switch failed (7)" reject, 07-12 eve): clamp the LO into
					 * the new band - selecting an antenna MOVES the view into its world */
					double nfreq = cfg->freq;
					{
						const struct rfnm_api_rx_ch *nrc = dev.get_rx_channel(pch);
						if (nrc && nrc->freq_max > nrc->freq_min && nrc->freq_max > 0) {
							if (nfreq < static_cast<double>(nrc->freq_min)) { nfreq = static_cast<double>(nrc->freq_min); }
							if (nfreq > static_cast<double>(nrc->freq_max)) { nfreq = static_cast<double>(nrc->freq_max); }
						}
					}
					dev.set_rx_channel_freq(nch.id, static_cast<uint64_t>(nfreq), false);
					dev.set_rx_channel_gain(nch.id, static_cast<int8_t>(cfg->gain), false);
					dev.set_rx_channel_agc(nch.id, RFNM_AGC_OFF, false);   /* leveling is host-side; the device field stays OFF */
					dev.set_rx_channel_path(nch.id, pth, false);
					dev.set_rx_channel_fe_mode(nch.id, fe_mode, false);   /* #39: the panorama contract moves with the session */
					{
						dev.set_rx_channel_rfic_lpf_bw(nch.id, rfic_lpf_for(cfg->rate), false);
					}
					dev.set_rx_channel_status(nch.id, RFNM_CH_RF_OFF, RFNM_CH_STREAM_AUTO, false);
					rfnm_api_failcode pret = dev.apply(nch.apply, true, 1000000);
					if (pret == RFNM_API_OK) {
						rx_ch = nch;
						rx_channel = pch;
						cfg->path = pstr;
						cfg->freq = nfreq;
						g_rec.cur_freq.store(static_cast<uint64_t>(cfg->freq), std::memory_order_relaxed);
						snprintf(g_rec.cur_path, sizeof(g_rec.cur_path), "%s", pstr);
						path_menu_set_current(rx_channel, cfg->path);
						path_persist_save(rx_channel, pstr);
						logts("antenna -> ch%u %s (LO %.3f MHz)", pch, device::rf_path_to_string(pth).c_str(), cfg->freq / 1e6);
					} else {
						/* stay on the old channel: re-arm its stream config + re-announce so the
						 * client dropdowns snap back to what the radio is actually doing */
						dev.set_rx_channel_status(rx_ch.id, RFNM_CH_RF_OFF, RFNM_CH_STREAM_AUTO, false);
						dev.apply(rx_ch.apply, true, 1000000);
						path_menu_set_current(rx_channel, cfg->path);
						logts("antenna channel switch failed (%d)", pret);
					}
					stream.reset(dev.rx_stream_create(rx_ch.mask));
					stream->set_auto_dc_offset(false);
					{
						int tries = 5;
						while (stream->start() != RFNM_API_OK && --tries && !g_stop) {
							std::this_thread::sleep_for(std::chrono::milliseconds(300));
							stream.reset(dev.rx_stream_create(rx_ch.mask));
							stream->set_auto_dc_offset(false);
						}
						if (!tries) { logts("antenna switch: RX stream restart failed after retries"); g_stop = 1; return; }
					}
					for (int i = 0; i < 8 && !g_stop; i++) { stream->read(bufs, chunk.size() / 2, elems, ts, 100000); }
					rt_gain = cfg->gain;   /* new channel/FE = new domain: reseed at the reference */
					rt_agc_blank(300);
					dcn_arm();           /* new channel/FE: full re-null */
					cur = nullptr;
					row_fill = 0;
				}
			}
			uint32_t nrate = tune->rate_req.exchange(0, std::memory_order_acq_rel);
			if (nrate && static_cast<double>(nrate) != cfg->rate) {
				/* span step: stream must be quiesced around the rate change (same discipline
				 * as the soapy fix). Within the 100M/2^k family this is decim-only: no DCS
				 * reclock, ~100-300ms gap. */
				logts("span step: %.2f -> %.2f MSPS", cfg->rate / 1e6, nrate / 1e6);
				stream->stop();
				rt_agc_blank(300);   /* rate/LPF change: never step across it */
				dcn_arm();           /* clock plan changes the DC too (287 vs 575 counts, P0) */
				rfnm_api_failcode rret = dev.set_samp_rate(nrate, 20000000);
				if (rret == RFNM_API_OK) {
					cfg->rate = static_cast<double>(nrate);
					g_rec.cur_rate.store(static_cast<uint32_t>(cfg->rate), std::memory_order_relaxed);
					/* narrow the analog front end with the span: an 80MHz-wide RFIC LPF at
					 * 25 MSPS aliases everything outside +-12.5M straight into the view */
					int16_t lpf_mhz = rfic_lpf_for(nrate);
					dev.set_rx_channel_rfic_lpf_bw(rx_ch.id, lpf_mhz, false);
					if (dev.apply(rx_ch.apply, true, 5000000) != RFNM_API_OK) { logts("lpf apply failed"); }
					else { logts("rfic lpf -> %d MHz", lpf_mhz); }
				}
				else { logts("rate change failed (%d), keeping %.2f MSPS", rret, cfg->rate / 1e6); }
				rret = stream->start();
				if (rret != RFNM_API_OK) {
					logts("stream restart fail %d — retrying once", rret);
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
					if (stream->start() != RFNM_API_OK) { g_stop = 1; return; }
				}
				cur = nullptr;
				row_fill = 0;
				for (int i = 0; i < 4 && !g_stop; i++) { stream->read(bufs, chunk.size() / 2, elems, ts, 100000); }
			}
			ret = stream->read(bufs, chunk.size() / 2, elems, ts, 200000);
			if (ret != RFNM_API_OK) { st->stream_errors++; continue; }
			st->rx_samples += elems;
			rt_agc_step(chunk.data(), elems);   /* host-side RT leveling (no-op when AGC off) */
			rt_dc_null(chunk.data(), elems);   /* one-shot analog trim (armed on discontinuities) */
			g_rec.push(chunk.data(), elems);   /* live-tap for the IQ recorder (no-op when idle) */
			size_t nwin = elems / N_FFT;
			if (st->clients.load(std::memory_order_relaxed) == 0) {
				static uint64_t last_warm = 0;
				uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
				if (now - last_warm < 1000) { continue; }   /* clientless: drain only (fans stay quiet) */
				last_warm = now;
			} else {
				/* demand-driven: produce ~1.25x what clients can consume; frees cores for the
				 * WS sender (a starved sender was the ~130 Mbps ceiling at "max") */
				uint32_t dem = st->demand_rows.load(std::memory_order_relaxed);
				if (dem < 60000) {
					static double credit = 0;
					static uint64_t last_ms = 0;
					uint64_t now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
					if (last_ms) { credit += (now_ms - last_ms) * dem * 1.25 / 1000.0; }
					last_ms = now_ms;
					double cap = dem * 1.25 / 4.0 + BATCH;
					if (credit > cap) { credit = cap; }
					/* produce PARTIAL reads: the read chunk varies with the rate plan (~64
					 * windows at 100M, ~10x that at 50M), so all-or-nothing gating starved
					 * low-cap clients (machine consumers ask for 2-60 rows/s; the GUI never
					 * goes that low) into the "RX flowing but no rows" watchdog death - take
					 * what credit affords, skip the rest of the read. With the demand floor
					 * above, production always outruns the 12 s watchdog. */
					if (credit < 1) { st->windows_skipped += nwin; continue; }
					size_t take = credit >= (double)nwin ? nwin : (size_t)credit;
					st->windows_skipped += nwin - take;
					credit -= (double)take;
					nwin = take;
				}
			}
			for (size_t w = 0; w < nwin; w++) {
				if (!cur) {
					cur = sp->acquire_free();
					if (!cur) { st->windows_skipped += nwin - w; break; }
					row_fill = 0;
				}
				memcpy(cur->buf + static_cast<size_t>(row_fill) * N_FFT * 2,
					chunk.data() + w * N_FFT * 2, static_cast<size_t>(N_FFT) * 4);
				if (++row_fill == BATCH) {
					/* the ring ticket is claimed only HERE, at batch completion - a half-filled
					 * slot holds NO ticket, so the discard sites (park/resume, sweep exit, antenna
					 * switch, span step) can drop it freely. Reserving at acquire time was defect
					 * #37's real mechanism (not galcore): stamp-gap partial reads at 100M leave
					 * the producer mid-batch at almost any loop-top, a discard then leaked the
					 * reserved ticket and published could never pass it - the whole pipeline
					 * starved while RX flowed (probe-proven: published froze at ticket*64, all
					 * slots free, watchdog 12 s later, 2026-07-12). */
					uint64_t t = rr->next_ticket.load(std::memory_order_relaxed);
					if (!rr->can_reserve(t)) { st->windows_skipped += BATCH; row_fill = 0; continue; }   /* ring full: recycle the slot in place */
					cur->ticket = t;
					rr->next_ticket.store(t + 1, std::memory_order_relaxed);
					cur->state.store(SLOT_FILLED, std::memory_order_release);
					cur = nullptr;
				}
			}
		}
		/* NULL when the loop exited while idle-PARKED (park resets the stream): this exact
		 * dereference was the teardown SEGV on every `systemctl stop` of a parked daemon -
		 * the appd idle-stop path - and the seed of crash-loop outages (start rate limit) */
		if (stream) { stream->stop(); }
	} catch (const std::exception &e) {
		logts("rx exception: %s", e.what());
		g_stop = 1;
	}
}
#endif

/* live-mode line ring: producers fold their (cache-hot) rows 4096->2048 right after
 * computing them; absolute row index keys the slot, WS only reads below rr->published.
 * 2048 lines = ~265ms of buffer at full rate, so the CFS transport thread never loses
 * lines to scheduling gaps (a 256-line WS-side ring lost 95% under load). */
#define LIVE_RING_G 2048
static uint8_t g_live_ring[LIVE_RING_G][N_FFT / 2];

static void fold_rows_live(const uint8_t *rows, uint64_t row0, int n) {
	for (int r = 0; r < n; r++) {
		const uint8_t *src = rows + static_cast<size_t>(r) * N_FFT;
		uint8_t *dst = g_live_ring[(row0 + static_cast<uint64_t>(r)) % LIVE_RING_G];
		for (int i = 0; i < N_FFT / 2; i++) {
			uint8_t a = src[2 * i], b = src[2 * i + 1];
			dst[i] = a > b ? a : b;
		}
	}
}

static void gpu_worker(GpuFFT *gpu, SlotPool *sp, RowRing *rr, Stats *st) {
	pin_self(2);
	int32_t cur_min = INT32_MIN, cur_max = INT32_MIN;
	while (!g_stop) {
		Slot *sl = sp->take_filled(0);
		// fast poll while streaming; back off hard when the RX is parked (no client) so idle
		// spectrumd doesn't spin the row ring - that residual kept the load ~2 at "idle"
		if (!sl) {
			bool parked = st->rx_parked.load(std::memory_order_relaxed);
			if (parked) {
				// keep the GL context warm across the park: galcore power-gates after long
				// idle and the first enqueue on resume can park forever (RT freeze class).
				// 0.5 s cadence: the 2 s cadence still wedged 2/13 resumes on 2026-07-12 -
				// the last poke before a resume must be recent enough to hold the clocks up.
				static int ka = 0;
				if (++ka >= 50) {   // every ~0.5 s of 10 ms polls
					ka = 0;
					gpu->keepalive();
				}
			}
			std::this_thread::sleep_for(std::chrono::microseconds(parked ? 10000 : 200));
			continue;
		}
		int32_t mn = g_scale->db_min_c.load(std::memory_order_relaxed);
		int32_t mx = g_scale->db_max_c.load(std::memory_order_relaxed);
		if (mn != cur_min || mx != cur_max) {
			cur_min = mn; cur_max = mx;
			gpu->set_scale(mn / 100.0f, mx / 100.0f);
			g_cfg->db_min = mn / 100.0f;
			g_cfg->db_max = mx / 100.0f;
		}
		if (!gpu->run(sl->gpu_idx, rr->row(sl->ticket * BATCH), BATCH)) { logts("gpu run failed"); g_stop = 1; return; }
		uint64_t t = sl->ticket;
		sl->state.store(SLOT_FREE, std::memory_order_release);
		if (g_dc_notch) { dc_notch_rows(rr->row(t * BATCH), BATCH); }
		fold_rows_live(rr->row(t * BATCH), t * BATCH, BATCH);
		rr->complete(t);
		st->rows_produced += BATCH;
		st->gpu_us_per_fft = static_cast<uint32_t>(gpu->avg_batch_ms * 1000.0f / BATCH);
	}
}

static void cpu_fft_worker(int idx, Cfg *cfg, SlotPool *sp, RowRing *rr, Stats *st) {
	pin_self(idx == 0 ? 0 : 2);          /* cpu0 (idle w/o USB) + cpu2 (GPU feeder mostly blocks in CL); cpu3 belongs to RX */
	PFFFT_Setup *setup = pffft_new_setup(N_FFT, PFFFT_COMPLEX);
	float *win = static_cast<float *>(pffft_aligned_malloc(N_FFT * 4));
	float *fin = static_cast<float *>(pffft_aligned_malloc(N_FFT * 8));
	float *fout = static_cast<float *>(pffft_aligned_malloc(N_FFT * 8));
	float *work = static_cast<float *>(pffft_aligned_malloc(N_FFT * 8));
	for (int i = 0; i < N_FFT; i++) { win[i] = 0.5f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) * i / (N_FFT - 1))); }
	while (!g_stop) {
		Slot *sl = sp->take_filled(1);
		if (!sl) { std::this_thread::sleep_for(std::chrono::microseconds(st->rx_parked.load(std::memory_order_relaxed) ? 10000 : 300)); continue; }
		const float dbMin = g_scale->db_min_c.load(std::memory_order_relaxed) / 100.0f + 72.25f;  /* + GPU fp16 scale offset */
		const float dbScale = 255.0f / (g_scale->db_max_c.load(std::memory_order_relaxed) / 100.0f - (dbMin - 72.25f));
		uint8_t *out = rr->row(sl->ticket * BATCH);
		for (int r = 0; r < BATCH; r++) {
			cw_convert(sl->buf + static_cast<size_t>(r) * N_FFT * 2, win, fin, N_FFT);
			pffft_transform_ordered(setup, fin, fout, work, PFFFT_FORWARD);
			cw_mag_u8(fout, out + static_cast<size_t>(r) * N_FFT, N_FFT, dbMin, dbScale);
		}
		uint64_t t = sl->ticket;
		sl->state.store(SLOT_FREE, std::memory_order_release);
		if (g_dc_notch) { dc_notch_rows(rr->row(t * BATCH), BATCH); }
		fold_rows_live(rr->row(t * BATCH), t * BATCH, BATCH);
		rr->complete(t);
		st->rows_produced += BATCH;
		st->cpu_batches++;
	}
	pffft_aligned_free(win); pffft_aligned_free(fin); pffft_aligned_free(fout); pffft_aligned_free(work);
	pffft_destroy_setup(setup);
}

/* ------------------------------------------------------------------ websocket server */

struct WsClient {
	int fd = -1;
	bool upgraded = false;
	std::string rxbuf;
	uint64_t next_row = 0;
	uint16_t max_rows = 3840;
	uint16_t mode = 0;           /* 0 raw, 1 avg, 2 fold (mean+max @ display rate), 3 live (every line, bins folded 2:1 to 2048 — raw's picture at half transport) */
	float ema_alpha = 0.1f;
	std::vector<float> ema;      /* avg mode accumulator */
	uint64_t last_send_ns = 0;
	uint32_t tx_seq = 0;
	double row_credit = 0;
	bool pmd = false;            /* permessage-deflate negotiated (server_no_context_takeover) */
	uint64_t next_line = 0;      /* live mode: cursor into the global pre-folded line ring */
	uint32_t live_bins = 2048;   /* zoom LOD: 2048/1024/512 */
	std::vector<uint8_t> txq;    /* pending wire bytes: sends are non-blocking, EPOLLOUT drains */
	size_t txoff = 0;
	bool dead = false;
	bool want_out = false;
	bool sweep = false;          /* swept-analyzer mode: served one full-span row per pass */
	uint32_t sweep_seq = 0;      /* last SweepOut.seq shipped to this client */
	uint32_t paths_gen = 0;      /* last PathMenu.gen announced to this client (0 = never) */
	Pacer pc;                    /* WAN flow control: see pacer.h (LAN: never engages) */
};

static double mono_ms() {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string b64(const uint8_t *in, size_t n) {
	static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string o;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] : 0) << 8 | (i + 2 < n ? in[i + 2] : 0);
		o += T[v >> 18 & 63];
		o += T[v >> 12 & 63];
		o += i + 1 < n ? T[v >> 6 & 63] : '=';
		o += i + 2 < n ? T[v & 63] : '=';
	}
	return o;
}

/* tiny SHA1 (for the WS accept key only) */
struct Sha1 {
	uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	static uint32_t rol(uint32_t x, int s) { return x << s | x >> (32 - s); }
	void digest(const uint8_t *msg, size_t len, uint8_t out[20]) {
		std::vector<uint8_t> m(msg, msg + len);
		uint64_t bits = static_cast<uint64_t>(len) * 8;
		m.push_back(0x80);
		while (m.size() % 64 != 56) { m.push_back(0); }
		for (int i = 7; i >= 0; i--) { m.push_back(static_cast<uint8_t>(bits >> (8 * i))); }
		for (size_t c = 0; c < m.size(); c += 64) {
			uint32_t w[80];
			for (int i = 0; i < 16; i++) { w[i] = m[c + 4 * i] << 24 | m[c + 4 * i + 1] << 16 | m[c + 4 * i + 2] << 8 | m[c + 4 * i + 3]; }
			for (int i = 16; i < 80; i++) { w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1); }
			uint32_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4];
			for (int i = 0; i < 80; i++) {
				uint32_t f, k;
				if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
				else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
				else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
				else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
				uint32_t t = rol(a, 5) + f + e + k + w[i];
				e = d; d = cc; cc = rol(b, 30); b = a; a = t;
			}
			h[0] += a; h[1] += b; h[2] += cc; h[3] += d; h[4] += e;
		}
		for (int i = 0; i < 5; i++) {
			out[4 * i] = static_cast<uint8_t>(h[i] >> 24);
			out[4 * i + 1] = static_cast<uint8_t>(h[i] >> 16);
			out[4 * i + 2] = static_cast<uint8_t>(h[i] >> 8);
			out[4 * i + 3] = static_cast<uint8_t>(h[i]);
		}
	}
};

static bool ws_send_frame_op(int fd, const uint8_t *payload, size_t n, uint8_t op0) {
	uint8_t hdr[10];
	size_t hl;
	hdr[0] = op0;
	if (n < 126) { hdr[1] = static_cast<uint8_t>(n); hl = 2; }
	else if (n < 65536) { hdr[1] = 126; hdr[2] = static_cast<uint8_t>(n >> 8); hdr[3] = static_cast<uint8_t>(n); hl = 4; }
	else { hdr[1] = 127; uint64_t v = n; for (int i = 0; i < 8; i++) { hdr[2 + i] = static_cast<uint8_t>(v >> (56 - 8 * i)); } hl = 10; }
	struct iovec iov[2] = {{hdr, hl}, {const_cast<uint8_t *>(payload), n}};
	struct msghdr mh = {};
	mh.msg_iov = iov;
	mh.msg_iovlen = 2;
	size_t want = hl + n, got = 0;
	while (got < want) {
		ssize_t w = sendmsg(fd, &mh, MSG_NOSIGNAL);
		if (w < 0) { return false; }
		got += static_cast<size_t>(w);
		/* advance iov */
		size_t adv = static_cast<size_t>(w);
		for (int i = 0; i < 2 && adv; i++) {
			size_t take = adv < iov[i].iov_len ? adv : iov[i].iov_len;
			iov[i].iov_base = static_cast<uint8_t *>(iov[i].iov_base) + take;
			iov[i].iov_len -= take;
			adv -= take;
		}
	}
	return true;
}

static bool ws_send_frame(int fd, const uint8_t *payload, size_t n) {
	return ws_send_frame_op(fd, payload, n, 0x82);
}

struct WsServer {
	Cfg *cfg;
	RowRing *rr;
	Stats *st;
	TuneBox *tune;
	ScaleBox *scale;
	SweepBox *sweep;
	/* global fold: every published row lands here once. Lines are cut by ROW COUNT
	 * (G rows/line = G*41us time slices, CE-grade waterfall time resolution) because
	 * the ring publishes in whole GPU batches (~134 rows) and wall-clock ticks cannot
	 * cut finer. The pump drains pending lines into multi-pair messages. */
	static constexpr int LIVE_WB = N_FFT / 2;
	z_stream zs = {};
	bool zinit = false;
	std::vector<uint8_t> zbuf;

	static constexpr int FOLD_G = 16;          /* 16 x 41us = 0.66ms per line */
	static constexpr int FOLD_MAXPEND = 64;
	uint64_t fold_next = 0;
	uint16_t fold_sum[N_FFT] = {};   /* resets every FOLD_G=16 rows: max 16*255=4080, u16-safe; u16 + split loops = NEON-vectorizable */
	uint8_t fold_max[N_FFT] = {};
	uint32_t fold_cnt = 0;
	uint8_t fold_lines[FOLD_MAXPEND][2][N_FFT];
	int fold_pend = 0;
	int lfd = -1, efd = -1;
	std::vector<WsClient *> clients;
	std::vector<uint8_t> frame;

	bool init() {
		lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
		int one = 1;
		setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		a.sin_port = htons(cfg->port);
		if (bind(lfd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) || listen(lfd, 8)) { logts("bind/listen failed"); return false; }
		efd = epoll_create1(0);
		epoll_event ev = {};
		ev.events = EPOLLIN;
		ev.data.ptr = nullptr;
		epoll_ctl(efd, EPOLL_CTL_ADD, lfd, &ev);
		frame.resize(RFSP_HDRSZ_SPECTRA + static_cast<size_t>(WS_ROWS_PER_MSG) * N_FFT);
		return true;
	}

	void put_hdr(uint8_t *p, uint8_t type, uint32_t seq, uint32_t paybytes) {
		struct rfsp_hdr h = {RFSP_MAGIC, RFSP_VERSION, type, 0, seq, paybytes};
		memcpy(p, &h, sizeof(h));
	}

	void handle_client_data(WsClient *c) {
		char buf[2048];
		ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
		if (n <= 0) { drop(c); return; }
		if (!c->upgraded) {
			c->rxbuf.append(buf, static_cast<size_t>(n));
			size_t hend = c->rxbuf.find("\r\n\r\n");
			if (hend == std::string::npos) { return; }
			size_t kp = c->rxbuf.find("Sec-WebSocket-Key:");
			if (kp == std::string::npos) { drop(c); return; }
			kp += 18;
			while (c->rxbuf[kp] == ' ') { kp++; }
			std::string key = c->rxbuf.substr(kp, c->rxbuf.find("\r\n", kp) - kp);
			key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
			uint8_t dg[20];
			Sha1().digest(reinterpret_cast<const uint8_t *>(key.data()), key.size(), dg);
			bool want_pmd = c->rxbuf.find("permessage-deflate") != std::string::npos;
			std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + b64(dg, 20) + "\r\n";
			if (want_pmd) {
				resp += "Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n";
				c->pmd = true;
			}
			resp += "\r\n";
			send(c->fd, resp.data(), resp.size(), MSG_NOSIGNAL);
			c->upgraded = true;
			/* single-viewer policy: the newest tab owns the radio; disconnect the rest */
			for (size_t ci = 0; ci < clients.size(); ci++) {
				if (clients[ci] != c && clients[ci]->upgraded) { clients[ci]->dead = true; }
			}
			/* the newest viewer starts in real-time; if it persisted sweep it re-arms via its
			 * own SWEEP message on connect (else a fresh RT tab would get no rows mid-sweep) */
			sweep->mode.store(0, std::memory_order_release);
			c->rxbuf.clear();
			c->next_row = rr->published.load(std::memory_order_acquire);
			c->max_rows = static_cast<uint16_t>(cfg->max_rows);
			logts("ws client connected (fd %d)", c->fd);
			return;
		}
		/* parse masked client frames (CONFIG/ACK/close/ping) */
		c->rxbuf.append(buf, static_cast<size_t>(n));
		while (c->rxbuf.size() >= 2) {
			const uint8_t *d = reinterpret_cast<const uint8_t *>(c->rxbuf.data());
			uint8_t op = d[0] & 0x0F;
			bool masked = d[1] & 0x80;
			uint64_t len = d[1] & 0x7F;
			size_t off = 2;
			if (len == 126) { if (c->rxbuf.size() < 4) { return; } len = d[2] << 8 | d[3]; off = 4; }
			else if (len == 127) { if (c->rxbuf.size() < 10) { return; } len = 0; for (int i = 0; i < 8; i++) { len = len << 8 | d[2 + i]; } off = 10; }
			size_t need = off + (masked ? 4 : 0) + len;
			if (c->rxbuf.size() < need) { return; }
			const uint8_t *mask = d + off;
			std::vector<uint8_t> pl(len);
			for (uint64_t i = 0; i < len; i++) { pl[i] = d[off + 4 + i] ^ (masked ? mask[i & 3] : 0); }
			if ((d[0] & 0x40) && c->pmd && len > 0) {   /* RSV1: per-message deflate from the client */
				pl.push_back(0x00); pl.push_back(0x00); pl.push_back(0xff); pl.push_back(0xff);
				std::vector<uint8_t> out(4096);
				z_stream is = {};
				if (inflateInit2(&is, -15) == Z_OK) {
					is.next_in = pl.data(); is.avail_in = static_cast<uInt>(pl.size());
					is.next_out = out.data(); is.avail_out = static_cast<uInt>(out.size());
					int zr = inflate(&is, Z_SYNC_FLUSH);
					size_t got = out.size() - is.avail_out;
					inflateEnd(&is);
					if (zr == Z_OK || zr == Z_STREAM_END || zr == Z_BUF_ERROR) { out.resize(got); pl.swap(out); len = pl.size(); }
				}
			}
			if (op == 8) { drop(c); return; }
			if (op == 9) { uint8_t pong[2] = {0x8A, 0}; send(c->fd, pong, 2, MSG_NOSIGNAL); }
			if (op == 2 && len >= 16) {
				const rfsp_hdr *h = reinterpret_cast<const rfsp_hdr *>(pl.data());
				/* LOUD ignores (rate-limited 1/s): a silently dropped control frame costs a debug
				 * round every time (the 2026-07-12 scale desync; short-struct third-party clients).
				 * One journal line names the frame and why it was ignored. */
				static uint64_t last_badfr_ms = 0;
				auto badframe = [&](const char *why) {
					uint64_t bnow = mono_ms();
					if (bnow - last_badfr_ms >= 1000) {
						last_badfr_ms = bnow;
						logts("ws client fd %d: control frame ignored (%s): type %u, %zu bytes", c->fd, why, static_cast<unsigned>(h->type), static_cast<size_t>(len));
					}
				};
				if (h->magic != RFSP_MAGIC) {
					badframe("bad magic");
				} else if (h->version != RFSP_VERSION) {
					badframe("protocol version mismatch");
				} else {
					static const struct { uint8_t type; size_t need; } fr_need[] = {
						{ RFSP_MSG_SCALE, sizeof(rfsp_hdr) + 2 * sizeof(float) }, { RFSP_MSG_TUNE, sizeof(rfsp_tune) },
						{ 20 /* RATE */, 20 }, { RFSP_MSG_SWEEP, sizeof(rfsp_sweep) }, { RFSP_MSG_CONFIG, sizeof(rfsp_config) },
						{ RFSP_MSG_ANTENNA, sizeof(rfsp_antenna) }, { RFSP_MSG_ACK, 24 },
					};
					for (const auto &fn : fr_need) {
						if (h->type == fn.type && len < fn.need) { badframe("short struct"); break; }
					}
				}
				if (h->magic == RFSP_MAGIC && h->version == RFSP_VERSION) {
					/* accept hdr + the two floats (24 B): the trailing reserved words are unread, and
					 * requiring the full 32 B silently ignored every short-struct client - the u8
					 * scale-window desync that corrupted A/B measurements (2026-07-12) */
					if (h->type == RFSP_MSG_SCALE && len >= sizeof(rfsp_hdr) + 2 * sizeof(float)) {
						const rfsp_scale *sc = reinterpret_cast<const rfsp_scale *>(pl.data());
						float mn = sc->db_min, mx = sc->db_max;
						if (mx - mn >= 5.0f && mx - mn <= 200.0f && mn > -200.0f && mx < 200.0f) {
							scale->db_min_c.store(static_cast<int32_t>(mn * 100), std::memory_order_relaxed);
							scale->db_max_c.store(static_cast<int32_t>(mx * 100), std::memory_order_relaxed);
						}
					}
					if (h->type == RFSP_MSG_TUNE && len >= sizeof(rfsp_tune)) {
						const rfsp_tune *tn = reinterpret_cast<const rfsp_tune *>(pl.data());
						if (tn->flags & 2) { tune->freq_mhz1000.store(static_cast<uint64_t>(tn->freq_hz / 1000.0), std::memory_order_release); }
						if (tn->flags & 1) { tune->gain.store(tn->gain_db, std::memory_order_release); }
						else if (tn->flags & 4) { tune->agc_req.store(tn->gain_db ? 1 : 0, std::memory_order_release); }   /* AGC toggle: gain_db carries 0/1 */
					}
					if (h->type == 20 && len >= 20) {   /* RATE: u32 Hz — zoom-driven RF span */
						uint32_t rq;
						memcpy(&rq, pl.data() + 16, 4);
						static const uint32_t ladder[] = {100000000u, 50000000u, 25000000u, 12500000u, 6250000u, 3125000u};
						for (uint32_t v : ladder) { if (rq == v) { tune->rate_req.store(rq, std::memory_order_release); break; } }
					}
					if (h->type == RFSP_MSG_SWEEP && len >= sizeof(rfsp_sweep)) {   /* enter/leave swept mode */
						const rfsp_sweep *sw = reinterpret_cast<const rfsp_sweep *>(pl.data());
						if (sw->enable) {
							static const uint32_t swladder[] = {100000000u, 50000000u, 25000000u, 12500000u, 6250000u, 3125000u};
							bool okbw = false;
							for (uint32_t v : swladder) { if (sw->bw_hz == v) { okbw = true; break; } }
							if (okbw && sw->stop_hz > sw->start_hz) {
								sweep->start_hz.store(static_cast<uint64_t>(sw->start_hz), std::memory_order_relaxed);
								sweep->stop_hz.store(static_cast<uint64_t>(sw->stop_hz), std::memory_order_relaxed);
								sweep->bw_hz.store(sw->bw_hz, std::memory_order_relaxed);
								sweep->gen.fetch_add(1, std::memory_order_relaxed);
								sweep->mode.store(1, std::memory_order_release);
								c->sweep = true;
								/* arm at the CURRENT seq: g_sweep_out survives sweep sessions, and sweep
								 * mode is force-zeroed on every ws upgrade, so seq!=0 here is always a
								 * DEAD session's last row (unbounded age, possibly other params) — wait
								 * for the first fresh pass instead of flashing it at connect */
								{ std::lock_guard<std::mutex> lk(g_sweep_out.mtx); c->sweep_seq = g_sweep_out.seq; }
							}
						} else {
							sweep->mode.store(0, std::memory_order_release);
							c->sweep = false;
						}
					}
					if (h->type == RFSP_MSG_CONFIG && len >= sizeof(rfsp_config)) {
						const rfsp_config *cf = reinterpret_cast<const rfsp_config *>(pl.data());
						/* SERVER-side floor at 16 rows/s (0 stays unthrottled): sub-16 caps starved the
						 * 50M plan's producer into the no-rows watchdog death (mechanism
						 * unpinned) - cap-16 is proven-working on every rate plan, so a
						 * low asker gets 16 (slightly more data, never a dead daemon). The rfnmctl
						 * clamp protects only rfnmctl; this protects every RFSP client. */
						uint16_t mr = cf->max_rows_per_sec;
						if (mr && mr < 16) {
							logts("ws client fd %d asked %u rows/s - floored to 16 (server minimum)", c->fd, static_cast<unsigned>(mr));
							mr = 16;
						}
						c->max_rows = mr ? mr : 65535;   /* 0 = unthrottled */
						c->mode = cf->mode;
						c->ema_alpha = cf->ema_alpha > 0 && cf->ema_alpha <= 1 ? cf->ema_alpha : 0.1f;
						c->live_bins = cf->fft_size_req;
						if (c->mode == 1 && c->ema.empty()) { c->ema.assign(N_FFT, 0.0f); }
					}
					if (h->type == RFSP_MSG_ANTENNA && len >= sizeof(rfsp_antenna)) {
						const rfsp_antenna *an = reinterpret_cast<const rfsp_antenna *>(pl.data());
						if (an->path <= 9 && an->channel < 8) { tune->path_req.store(static_cast<int32_t>(an->channel) << 8 | an->path, std::memory_order_release); }
					}
					if (h->type == RFSP_MSG_ACK && len >= 24) {
						/* client echoes the seq of the last SPECTRA it processed (one ACK
						 * per message); feeds the BDP window - see pacer.h */
						uint32_t aseq;
						memcpy(&aseq, pl.data() + 16, 4);
						c->pc.on_ack(aseq, mono_ms());
					}
				}
			}
			c->rxbuf.erase(0, need);
		}
	}

	void drop(WsClient *c) {
		epoll_ctl(efd, EPOLL_CTL_DEL, c->fd, nullptr);
		close(c->fd);
		logts("ws client dropped (fd %d)", c->fd);
		if (c->sweep) { sweep->mode.store(0, std::memory_order_release); }   /* don't keep sweeping with no viewer */
		for (auto it = clients.begin(); it != clients.end(); ++it) {
			if (*it == c) { clients.erase(it); break; }
		}
		st->clients.store(static_cast<int>(clients.size()), std::memory_order_relaxed);
		delete c;
	}

	/* announce the RX path menu (rfsp_paths) when the client hasn't seen the current gen —
	 * covers connect AND any path/channel change (other viewers, daemon-side switches) */
	void send_paths(WsClient *c) {
		uint32_t gen = g_pathmenu.gen.load(std::memory_order_acquire);
		if (gen == c->paths_gen) { return; }
		struct rfsp_paths pm;
		memset(&pm, 0, sizeof(pm));
		{
			std::lock_guard<std::mutex> lk(g_pathmenu.mtx);
			memcpy(pm.dgb_name, g_pathmenu.dgb_name, sizeof(pm.dgb_name));
			pm.count = static_cast<uint8_t>(g_pathmenu.entries.size() < RFSP_PATHS_MAX ? g_pathmenu.entries.size() : RFSP_PATHS_MAX);
			pm.current_idx = 0;
			for (uint8_t i = 0; i < pm.count; i++) {
				pm.entry[i].code = g_pathmenu.entries[i].code;
				pm.entry[i].channel = g_pathmenu.entries[i].channel;
				pm.entry[i].dgb = g_pathmenu.entries[i].dgb;
				pm.entry[i].fmin_mhz = g_pathmenu.entries[i].fmin_mhz;
				pm.entry[i].fmax_mhz = g_pathmenu.entries[i].fmax_mhz;
				if (g_pathmenu.entries[i].code == g_pathmenu.cur_code && g_pathmenu.entries[i].channel == g_pathmenu.cur_channel) { pm.current_idx = i; }
			}
		}
		if (!pm.count) { return; }   /* menu not enumerated yet (rx thread still starting) */
		put_hdr(reinterpret_cast<uint8_t *>(&pm), RFSP_MSG_PATHS, c->tx_seq++, sizeof(pm) - sizeof(rfsp_hdr));
		if (enqueue_frame(c, reinterpret_cast<uint8_t *>(&pm), sizeof(pm), 0x82)) { c->paths_gen = gen; }
	}

	void pump_rows(double dt) {
		uint64_t head = rr->published.load(std::memory_order_acquire);
		/* advance the global fold over newly published rows (once, all clients share it) */
		{
			bool any_fold = false;
			for (auto *c : clients) { if (c->upgraded && c->mode >= 2) { any_fold = true; break; } }
			if (any_fold) {
				if (head > fold_next + ROW_RING - BATCH) { fold_next = head - BATCH; }
				for (uint64_t r = fold_next; r < head; r++) {
					const uint8_t *row = rr->row(r);
					/* two branch-free passes: gcc auto-vectorizes both (the old mixed
					 * scalar loop cost ~66M ops/s and starved this thread's pump) */
					for (int i = 0; i < N_FFT; i++) { fold_sum[i] = static_cast<uint16_t>(fold_sum[i] + row[i]); }
					for (int i = 0; i < N_FFT; i++) { fold_max[i] = row[i] > fold_max[i] ? row[i] : fold_max[i]; }
					if (++fold_cnt >= FOLD_G) {
						if (fold_pend < FOLD_MAXPEND) {
							uint8_t *mean = fold_lines[fold_pend][0], *mx = fold_lines[fold_pend][1];
							for (int i = 0; i < N_FFT; i++) {
								mean[i] = static_cast<uint8_t>(fold_sum[i] / fold_cnt);
								mx[i] = fold_max[i];
							}
							fold_pend++;
						}
						fold_cnt = 0;
						memset(fold_sum, 0, sizeof(fold_sum));
						memset(fold_max, 0, sizeof(fold_max));
					}
				}
				fold_next = head;
			} else {
				fold_next = head;
				fold_pend = 0;
			}
		}
		const double now_ms = mono_ms();
		for (size_t ci = 0; ci < clients.size();) {
			WsClient *c = clients[ci];
			if (!c->upgraded) { ci++; continue; }
			c->pc.tick(now_ms);
			send_paths(c);
			if (c->sweep) {
				/* swept mode: one full-span row per completed sweep pass (run_sweep publishes
				 * into g_sweep_out). The RT ring/credit machinery is bypassed for this client. */
				bool have_new = false;
				double center = 0, span = 0;
				uint32_t sq = 0;
				{
					std::lock_guard<std::mutex> lk(g_sweep_out.mtx);
					sq = g_sweep_out.seq;
					if (sq != c->sweep_seq) {
						memcpy(frame.data() + RFSP_HDRSZ_SPECTRA, g_sweep_out.row, N_FFT);
						center = g_sweep_out.center_hz;
						span = g_sweep_out.span_hz;
						have_new = true;
					}
				}
				bool alive = true;
				if (have_new && !c->pc.win_blocked(now_ms)) {   /* blocked: retry next pump (sweep_seq unmoved) */
					uint8_t *p = frame.data();
					uint32_t wseq = tx_seq_shared++;
					put_hdr(p, RFSP_MSG_SPECTRA, wseq, 48 + N_FFT);
					fill_spectra_hdr_cs(p, 1, center, span);
					z_last_p = nullptr;
					alive = send_msg(c, p, RFSP_HDRSZ_SPECTRA + N_FFT);
					if (alive) {
						c->sweep_seq = sq;
						st->rows_sent += 1;
						if (last_wire_n) { c->pc.on_send(wseq, last_wire_n, now_ms); }
					}
				}
				c->next_row = head;   /* keep the RT cursor pinned so the fell-behind clamps stay quiet */
				if (!alive) { drop(c); continue; }
				ci++;
				continue;
			}
			if (head > c->next_row + ROW_RING - BATCH) {           /* fell behind the ring */
				st->rows_dropped += head - BATCH - c->next_row;
				c->next_row = head - BATCH;
			}
			c->row_credit += dt * c->max_rows;
			if (c->row_credit > c->max_rows / 4.0 + WS_ROWS_PER_MSG) { c->row_credit = c->max_rows / 4.0 + WS_ROWS_PER_MSG; }
			bool alive = true;
			if (c->mode == 3) {
				/* zoom LOD: client lowers bins (CONFIG fft_size: 2048/1024/512) and line
				 * rate (max_rows) as it zooms — bandwidth drops per zoom step, full span always */
				/* live: every line from the global pre-folded ring (fold ran once for all
				 * clients); compressed via send_msg when the client negotiated deflate. */
				int WB = LIVE_WB;
				if (c->live_bins == 1024 || c->live_bins == 512) { WB = c->live_bins; }
				const int fold2 = LIVE_WB / WB;                     /* 1, 2 or 4 : further max-fold at pack */
				int LINES_PER_MSG = 57344 / WB;                     /* ~56KB payload cap */
				if (c->pc.limited()) {
					/* paced link: a 56 KB message is 0.35 s of WIRE TIME at 1.3 Mbit/s - data
					 * lands in coarse bursts and the GUI steps instead of flows (round-5 report).
					 * Small messages give the pacer fine-grained scheduling; header overhead ~1%. */
					const int cap = 8192 / WB > 1 ? 8192 / WB : 1;
					if (LINES_PER_MSG > cap) { LINES_PER_MSG = cap; }
				}
				if (c->next_row + LIVE_RING_G - 64 < head) {        /* fell behind the ring */
					st->rows_dropped += head - 64 - c->next_row;
					c->next_row = head - 64;
				}
				while (alive && head > c->next_row && c->row_credit >= 1.0 && !c->pc.win_blocked(now_ms)) {
					int k = static_cast<int>(head - c->next_row);
					if (k > LINES_PER_MSG) { k = LINES_PER_MSG; }
					if (k > static_cast<int>(c->row_credit)) { k = static_cast<int>(c->row_credit); }
					uint8_t *p = frame.data();
					uint32_t lseq = tx_seq_shared++;
					put_hdr(p, RFSP_MSG_SPECTRA, lseq, 48 + static_cast<uint32_t>(k) * WB);
					fill_spectra_hdr(p, static_cast<uint16_t>(k));
					uint16_t m3 = 3;
					uint32_t wb32 = static_cast<uint32_t>(WB);
					memcpy(p + 16 + 0, &wb32, 4);                   /* n_bins = line width */
					memcpy(p + 16 + 6, &m3, 2);
					uint8_t *pay = p + RFSP_HDRSZ_SPECTRA;
					for (int l = 0; l < k; l++) {
						const uint8_t *src = g_live_ring[(c->next_row + static_cast<uint64_t>(l)) % LIVE_RING_G];
						uint8_t *dst = pay + static_cast<size_t>(l) * WB;
						if (fold2 == 1) { memcpy(dst, src, WB); }
						else {
							for (int i = 0; i < WB; i++) {
								uint8_t m = src[fold2 * i];
								for (int j = 1; j < fold2; j++) { if (src[fold2 * i + j] > m) { m = src[fold2 * i + j]; } }
								dst[i] = m;
							}
						}
					}
					/* live lines ship UNCOMPRESSED at full rate: one zlib stream on a
					 * contended A53 caps at ~12 MB/s and the full line rate is ~17 MB/s —
					 * deflate here throttled the browser to 300 lines/s. Bin-halving is the
					 * live bandwidth story; pairs/raw stay compressed. EXCEPT when the
					 * pacer says the link is the bottleneck: then the byte rate is at most
					 * the link rate (far under the zlib ceiling) and deflate is ~1.5x free
					 * lines for the WAN viewer. */
					if (c->pmd && c->pc.limited()) {
						z_last_p = nullptr;   /* per-client cursor content: no broadcast reuse */
						alive = send_msg(c, p, RFSP_HDRSZ_SPECTRA + static_cast<size_t>(k) * WB);
					} else {
						alive = enqueue_frame(c, p, RFSP_HDRSZ_SPECTRA + static_cast<size_t>(k) * WB, 0x82);
					}
					if (alive) {
						c->next_row += static_cast<uint64_t>(k);
						c->row_credit -= k;
						st->rows_sent += static_cast<uint64_t>(k);
						if (last_wire_n) { c->pc.on_send(lseq, last_wire_n, now_ms); }
					}
				}
				/* rate-capped or window-blocked: skip forward, stay fresh */
				if (head > c->next_row && (c->row_credit < 1.0 || c->pc.win_blocked(now_ms))) { c->next_row = head; }
						} else if (c->mode == 2) {
				c->next_row = head;   /* fold clients are served by fold_tick() */
			} else if (c->mode == 1) {
				/* AVG: fold all new rows into EMA, emit at most one row per pump when credit allows */
				for (uint64_t r = c->next_row; r < head; r++) {
					const uint8_t *row = rr->row(r);
					for (int i = 0; i < N_FFT; i++) { c->ema[i] += c->ema_alpha * (row[i] - c->ema[i]); }
				}
				c->next_row = head;
				if (c->row_credit >= 1.0 && head && !c->pc.win_blocked(now_ms)) {
					uint8_t *p = frame.data();
					uint32_t aseq = c->tx_seq++;
					put_hdr(p, RFSP_MSG_SPECTRA, aseq, 48 + N_FFT);
					fill_spectra_hdr(p, 1);
					uint8_t *pay = p + RFSP_HDRSZ_SPECTRA;
					for (int i = 0; i < N_FFT; i++) { pay[i] = static_cast<uint8_t>(c->ema[i]); }
					z_last_p = nullptr;
					alive = send_msg(c, p, RFSP_HDRSZ_SPECTRA + N_FFT);
					c->row_credit = 0;
					if (alive) {
						st->rows_sent += 1;
						if (last_wire_n) { c->pc.on_send(aseq, last_wire_n, now_ms); }
					}
				}
			} else {
				int msgs_this_pump = 0;
				while (alive && head - c->next_row >= WS_ROWS_PER_MSG && c->row_credit >= WS_ROWS_PER_MSG && msgs_this_pump++ < 4 && !c->pc.win_blocked(now_ms)) {
					uint8_t *p = frame.data();
					uint32_t rseq = c->tx_seq++;
					put_hdr(p, RFSP_MSG_SPECTRA, rseq, 48 + WS_ROWS_PER_MSG * N_FFT);
					fill_spectra_hdr(p, WS_ROWS_PER_MSG);
					uint8_t *pay = p + RFSP_HDRSZ_SPECTRA;
					for (int r = 0; r < WS_ROWS_PER_MSG; r++) {
						memcpy(pay + static_cast<size_t>(r) * N_FFT, rr->row(c->next_row + static_cast<uint64_t>(r)), N_FFT);
					}
					z_last_p = nullptr;
					alive = send_msg(c, p, RFSP_HDRSZ_SPECTRA + static_cast<size_t>(WS_ROWS_PER_MSG) * N_FFT);
					if (alive) {
						c->next_row += WS_ROWS_PER_MSG;
						c->row_credit -= WS_ROWS_PER_MSG;
						st->rows_sent += WS_ROWS_PER_MSG;
						if (last_wire_n) { c->pc.on_send(rseq, last_wire_n, now_ms); }
					}
				}
				/* if pipeline outruns the client cap (or the link window), jump forward (decimate) */
				if (head > c->next_row && (c->row_credit < WS_ROWS_PER_MSG || c->pc.win_blocked(now_ms))) {
					uint64_t skip = head - c->next_row;
					if (skip > WS_ROWS_PER_MSG * 4) {
						st->rows_dropped += skip - WS_ROWS_PER_MSG;
						c->next_row = head - WS_ROWS_PER_MSG;
					}
				}
			}
			if (!alive) { drop(c); continue; }
			ci++;
		}
	}

	/* non-blocking sender: whole frames append to the client queue; MSG_DONTWAIT drain
	 * here and on EPOLLOUT. A slow consumer fills its queue and starts LOSING messages
	 * (mode cursors resync via the fell-behind clamps) instead of stalling the pump —
	 * the blocking sendmsg stalled the whole server 180ms per send under a slow client. */
	void flush_client(WsClient *c) {
		while (c->txoff < c->txq.size()) {
			ssize_t w = send(c->fd, c->txq.data() + c->txoff, c->txq.size() - c->txoff, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (w > 0) { c->txoff += static_cast<size_t>(w); continue; }
			if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				if (!c->want_out) {
					epoll_event ev = {};
					ev.events = EPOLLIN | EPOLLOUT;
					ev.data.ptr = c;
					epoll_ctl(efd, EPOLL_CTL_MOD, c->fd, &ev);
					c->want_out = true;
				}
				return;
			}
			c->dead = true;
			return;
		}
		c->txq.clear();
		c->txoff = 0;
		if (c->want_out) {
			epoll_event ev = {};
			ev.events = EPOLLIN;
			ev.data.ptr = c;
			epoll_ctl(efd, EPOLL_CTL_MOD, c->fd, &ev);
			c->want_out = false;
		}
	}

	size_t last_wire_n = 0;   /* wire bytes of the last enqueue_frame (0 = message was dropped) */

	bool enqueue_frame(WsClient *c, const uint8_t *payload, size_t n, uint8_t op0) {
		if (c->dead) { return false; }
		last_wire_n = 0;
		if (c->txq.size() - c->txoff > (1u << 20)) { c->pc.on_drop(); return true; }   /* lagging: lose this message, keep the client */
		last_wire_n = n;   /* header bytes excluded: close enough, and consistent across paths */
		uint8_t hdr[10];
		size_t hl;
		hdr[0] = op0;
		if (n < 126) { hdr[1] = static_cast<uint8_t>(n); hl = 2; }
		else if (n < 65536) { hdr[1] = 126; hdr[2] = static_cast<uint8_t>(n >> 8); hdr[3] = static_cast<uint8_t>(n); hl = 4; }
		else { hdr[1] = 127; uint64_t v = n; for (int i = 0; i < 8; i++) { hdr[2 + i] = static_cast<uint8_t>(v >> (56 - 8 * i)); } hl = 10; }
		if (c->txoff > 0 && c->txoff == c->txq.size()) { c->txq.clear(); c->txoff = 0; }
		if (c->txq.empty()) {
			/* fast path: direct non-blocking writev, buffer only the unsent tail —
			 * routing everything through txq double-copied every byte */
			struct iovec iov[2] = {{hdr, hl}, {const_cast<uint8_t *>(payload), n}};
			struct msghdr mh = {};
			mh.msg_iov = iov;
			mh.msg_iovlen = 2;
			ssize_t w = sendmsg(c->fd, &mh, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { c->dead = true; return false; }
			size_t sent = w > 0 ? static_cast<size_t>(w) : 0;
			if (sent >= hl + n) { return true; }
			if (sent < hl) { c->txq.insert(c->txq.end(), hdr + sent, hdr + hl); c->txq.insert(c->txq.end(), payload, payload + n); }
			else { c->txq.insert(c->txq.end(), payload + (sent - hl), payload + n); }
			flush_client(c);
			return !c->dead;
		}
		c->txq.insert(c->txq.end(), hdr, hdr + hl);
		c->txq.insert(c->txq.end(), payload, payload + n);
		flush_client(c);
		return !c->dead;
	}

	/* one message, compressed once (shared stream, reset per message = the negotiated
	 * server_no_context_takeover), broadcast-safe: repeated calls with the same buffer
	 * reuse the compressed copy. Small messages go plain. */
	bool send_msg(WsClient *c, const uint8_t *p, size_t n) {
		if (!c->pmd || n < 512) { return enqueue_frame(c, p, n, 0x82); }
		if (!zinit) {
			if (deflateInit2(&zs, 1, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) { return enqueue_frame(c, p, n, 0x82); }
			zinit = true;
			zbuf.resize(RFSP_HDRSZ_SPECTRA + static_cast<size_t>(WS_ROWS_PER_MSG) * N_FFT + 2048);
		}
		if (p != z_last_p || n != z_last_n) {
			deflateReset(&zs);
			zs.next_in = const_cast<uint8_t *>(p);
			zs.avail_in = static_cast<uInt>(n);
			zs.next_out = zbuf.data();
			zs.avail_out = static_cast<uInt>(zbuf.size());
			if (deflate(&zs, Z_SYNC_FLUSH) != Z_OK || zs.avail_out < 4) { z_last_p = nullptr; return enqueue_frame(c, p, n, 0x82); }
			z_last_zn = zbuf.size() - zs.avail_out - 4;   /* strip 00 00 ff ff */
			z_last_p = p; z_last_n = n;
		}
		return enqueue_frame(c, zbuf.data(), z_last_zn, 0xC2);   /* FIN|RSV1|binary */
	}
	const uint8_t *z_last_p = nullptr;
	size_t z_last_n = 0, z_last_zn = 0;
	uint32_t tx_seq_shared = 0;
	uint64_t dbg_pump_us = 0, dbg_pumps = 0;

	void fold_tick() {
		if (fold_pend == 0) { return; }
		bool any = false;
		for (auto *c : clients) { if (c->upgraded && c->mode >= 2) { any = true; break; } }
		if (!any) { fold_pend = 0; return; }
		int done = 0;
		while (done < fold_pend) {
			int k = fold_pend - done;
			if (k > 7) { k = 7; }   /* 7 pairs = 57KB fits the 64KB frame buffer */

			uint8_t *p = frame.data();
			uint32_t fseq = tx_seq_shared++;
			put_hdr(p, RFSP_MSG_SPECTRA, fseq, 48 + 2 * k * N_FFT);
			fill_spectra_hdr(p, static_cast<uint16_t>(2 * k));
			uint16_t m2 = 2;
			uint32_t g = FOLD_G;
			memcpy(p + 16 + 6, &m2, 2);
			memcpy(p + 16 + 36, &g, 4);
			uint8_t *pay = p + RFSP_HDRSZ_SPECTRA;
			for (int l = 0; l < k; l++) {
				memcpy(pay + (2 * l) * N_FFT, fold_lines[done + l][0], N_FFT);
				memcpy(pay + (2 * l + 1) * N_FFT, fold_lines[done + l][1], N_FFT);
			}
			z_last_p = nullptr;   /* new content: compress once, reuse for every client */
			const double fnow = mono_ms();
			for (size_t ci = 0; ci < clients.size();) {
				WsClient *c = clients[ci];
				if (c->upgraded && c->mode >= 2) {
					if (c->pc.limited()) { ci++; continue; }   /* paced: served the single-pair tail below */
					if (c->pc.win_blocked(fnow)) { ci++; continue; }   /* link-full: this client skips the pair */
					if (!send_msg(c, p, RFSP_HDRSZ_SPECTRA + 2 * static_cast<size_t>(k) * N_FFT)) { drop(c); continue; }
					st->rows_sent += 2 * k;
					if (last_wire_n) { c->pc.on_send(fseq, last_wire_n, fnow); }
				}
				ci++;
			}
			done += k;
		}
		/* paced clients get ONE pair - the freshest - per tick instead of 57 KB batches:
		 * an 8 KB unit the pacer can actually schedule between live-line messages, so the
		 * exact-level trace source keeps a usable cadence on a slow link instead of being
		 * the first casualty of flow control */
		{
			bool any_lim = false;
			for (auto *c : clients) { if (c->upgraded && c->mode >= 2 && c->pc.limited()) { any_lim = true; break; } }
			if (any_lim) {
				uint8_t *p = frame.data();
				uint32_t fseq = tx_seq_shared++;
				put_hdr(p, RFSP_MSG_SPECTRA, fseq, 48 + 2 * N_FFT);
				fill_spectra_hdr(p, 2);
				uint16_t m2 = 2;
				uint32_t g = FOLD_G;
				memcpy(p + 16 + 6, &m2, 2);
				memcpy(p + 16 + 36, &g, 4);
				memcpy(p + RFSP_HDRSZ_SPECTRA, fold_lines[fold_pend - 1][0], N_FFT);
				memcpy(p + RFSP_HDRSZ_SPECTRA + N_FFT, fold_lines[fold_pend - 1][1], N_FFT);
				z_last_p = nullptr;
				const double fnow = mono_ms();
				for (size_t ci = 0; ci < clients.size();) {
					WsClient *c = clients[ci];
					if (c->upgraded && c->mode >= 2 && c->pc.limited() && !c->pc.win_blocked(fnow)) {
						if (!send_msg(c, p, RFSP_HDRSZ_SPECTRA + 2 * static_cast<size_t>(N_FFT))) { drop(c); continue; }
						st->rows_sent += 2;
						if (last_wire_n) { c->pc.on_send(fseq, last_wire_n, fnow); }
					}
					ci++;
				}
			}
		}
		fold_pend = 0;
	}

	void fill_spectra_hdr(uint8_t *p, uint16_t nrows) {
		/* db window from the LIVE atomics, not the static cfg: the engines quantize with the
		 * atomics, so a header reporting anything else lies to every external decoder (found
		 * as the study-day scale-window desync; cfg->db_min was start-of-day state in RT mode) */
		struct __attribute__((packed)) {
			uint32_t n_bins;
			uint16_t n_rows, mode;
			float db_min, db_max;
			double center_hz, span_hz;
			uint32_t fft_size, accum, src_rows, resv;
		} sh = {N_FFT, nrows, 0, scale->db_min_c.load(std::memory_order_relaxed) / 100.0f,
			scale->db_max_c.load(std::memory_order_relaxed) / 100.0f, cfg->freq, cfg->rate, N_FFT, 1,
			static_cast<uint32_t>(st->rows_produced.load()), 0};
		memcpy(p + 16, &sh, sizeof(sh));
	}

	/* like fill_spectra_hdr but with an explicit center/span (swept mode: the row spans the
	 * whole sweep range, not the instantaneous cfg->freq/rate). mode stays 0 so the client
	 * renders it as an ordinary trace/waterfall line positioned by the header center/span. */
	void fill_spectra_hdr_cs(uint8_t *p, uint16_t nrows, double center, double span) {
		struct __attribute__((packed)) {
			uint32_t n_bins;
			uint16_t n_rows, mode;
			float db_min, db_max;
			double center_hz, span_hz;
			uint32_t fft_size, accum, src_rows, resv;
		} sh = {N_FFT, nrows, 0, scale->db_min_c.load(std::memory_order_relaxed) / 100.0f,
			scale->db_max_c.load(std::memory_order_relaxed) / 100.0f, center, span, N_FFT, 1,
			static_cast<uint32_t>(st->rows_produced.load()), 0};
		memcpy(p + 16, &sh, sizeof(sh));
	}

	void send_status() {
		static uint64_t last_prod = 0, last_sent = 0, last_drop = 0;
		uint64_t p = st->rows_produced.load(), s = st->rows_sent.load(), d = st->rows_dropped.load() + st->windows_skipped.load();
		uint8_t msg[64];
		put_hdr(msg, RFSP_MSG_STATUS, 0, 48);
		struct __attribute__((packed)) {
			uint32_t fps, sent, dropped;
			float dsp_us, cpu, dsp_load;
			uint32_t r0, r1;
			uint32_t link_limited, link_kbps, link_rtt_ms, agc_word;   /* per-client pacer tail + AGC state */
		} sb = {static_cast<uint32_t>(p - last_prod), static_cast<uint32_t>(s - last_sent), static_cast<uint32_t>(d - last_drop),
			static_cast<float>(st->gpu_us_per_fft.load()), 0, 0,
			static_cast<uint32_t>(dbg_pumps), static_cast<uint32_t>(dbg_pumps ? dbg_pump_us / dbg_pumps : 0),
			0, 0, 0,
			/* byte 0 = live gain (i8: the RT stepper's gain, reference gain in sweep),
			 * byte 1 = AGC enabled; the old reserved2 word, zero on pre-AGC servers */
			(static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int8_t>(g_agc_gain.load(std::memory_order_relaxed))))) |
			(g_agc_on.load(std::memory_order_relaxed) ? 0x100u : 0u)};
		dbg_pumps = 0; dbg_pump_us = 0;
		last_prod = p; last_sent = s; last_drop = d;
		for (auto *c : clients) {
			if (!c->upgraded) { continue; }
			sb.link_limited = c->pc.limited() ? 1 : 0;
			sb.link_kbps = c->pc.deliv_kbps();
			sb.link_rtt_ms = static_cast<uint32_t>(c->pc.rtt_min_ms);
			memcpy(msg + 16, &sb, sizeof(sb));
			enqueue_frame(c, msg, sizeof(msg), 0x82);
		}
	}

	void run() {
		/* CFS with boosted weight: FIFO starved the engine (dsp 6.9k->2k), plain CFS
		 * starved this pump (2.5k of 7.5k lines/s delivered). nice -10 triples the CFS
		 * share without hard starvation either way. */
		if (nice(-10) == -1) { /* best effort */ }
		auto last_status = std::chrono::steady_clock::now();
		auto last_pump = last_status;
		while (!g_stop) {
			epoll_event evs[16];
			int n = epoll_wait(efd, evs, 16, 2);
			for (int i = 0; i < n; i++) {
				if (evs[i].data.ptr == nullptr) {
					int fd = accept4(lfd, nullptr, nullptr, 0);
					if (fd < 0) { continue; }
					int one = 1;
					setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
					auto *c = new WsClient();
					c->fd = fd;
					clients.push_back(c);
					epoll_event ev = {};
					ev.events = EPOLLIN;
					ev.data.ptr = c;
					epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
				} else {
					auto *c = static_cast<WsClient *>(evs[i].data.ptr);
					if (evs[i].events & EPOLLOUT) { flush_client(c); }
					if (evs[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) { handle_client_data(c); }
				}
			}
			st->clients.store(static_cast<int>(clients.size()), std::memory_order_relaxed);
			{
				uint32_t dem = 0;
				for (auto *c : clients) { uint32_t m = (c->mode >= 2) ? 7200 : c->max_rows;   /* just above the ~6.9k ceiling: sustained truly-uncapped GPU wedged the Vivante driver */ if (m > dem) { dem = m; } }
				/* floor the aggregate: production is batch-quantized (64 rows), so a tiny cap
				 * (machine clients may ask for 2 rows/s) starves whole batches past the 12 s
				 * no-rows watchdog and restarts the daemon. The per-client throttle still
				 * delivers only what each client asked for. */
				if (dem && dem < 16) { dem = 16; }
				st->demand_rows.store(dem ? dem : 3840, std::memory_order_relaxed);
			}
			auto now = std::chrono::steady_clock::now();
			double dt = std::chrono::duration<double>(now - last_pump).count();
			last_pump = now;
			{
				auto tp0 = std::chrono::steady_clock::now();
				for (size_t ci = 0; ci < clients.size();) {
				if (clients[ci]->dead) { drop(clients[ci]); continue; }
				ci++;
			}
			pump_rows(dt);
				fold_tick();
				dbg_pump_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tp0).count());
				dbg_pumps++;
			}
			if (now - last_status > std::chrono::seconds(1)) {
				send_status();
				last_status = now;
				/* GC7000 wedge watchdog: the Vivante stack occasionally parks a CL wait forever
				 * (threads stuck in gckOS_WaitSignal), freezing the in-order row ring - RT goes
				 * black until a restart while RX keeps flowing. Detect "clients watching, RX
				 * moving, zero rows for 12 s in real-time mode" and exit(1): systemd's
				 * Restart=on-failure turns a dead GUI into a ~2 s blip. */
				static uint64_t wd_rows = 0, wd_rx = 0;
				static int wd_strikes = 0;
				uint64_t rp = st->rows_produced.load(std::memory_order_relaxed);
				uint64_t rx = st->rx_samples.load(std::memory_order_relaxed);
				bool rt_active = st->clients.load(std::memory_order_relaxed) > 0 &&
						!st->rx_parked.load(std::memory_order_relaxed) &&
						!sweep->mode.load(std::memory_order_relaxed);
				if (rt_active && rp == wd_rows && rx != wd_rx) {
					if (++wd_strikes >= 12) {
						logts("watchdog: RX flowing but no rows produced for %ds - exiting for restart "
								"(published=%llu next_ticket=%llu slots=[%d%d%d%d] cpu_batches=%llu: "
								"published<next*64 w/ free slots = lost ring ticket; a BUSY gpu slot pinning published = CL wedge)",
								wd_strikes,
								(unsigned long long)rr->published.load(std::memory_order_relaxed),
								(unsigned long long)rr->next_ticket.load(std::memory_order_relaxed),
								g_slots->slots[0].state.load(std::memory_order_relaxed), g_slots->slots[1].state.load(std::memory_order_relaxed),
								g_slots->slots[2].state.load(std::memory_order_relaxed), g_slots->slots[3].state.load(std::memory_order_relaxed),
								(unsigned long long)st->cpu_batches.load(std::memory_order_relaxed));
						/* _exit, not exit: atexit/static teardown walks into the wedged GL stack and
						 * HANGS (measured: a zombie daemon holding the port+lease, GUI unrecoverable
						 * until a manual kill -9 - exactly the failure this watchdog exists to fix) */
						_exit(1);
					}
				} else {
					wd_strikes = 0;
				}
				wd_rows = rp;
				wd_rx = rx;
			}
		}
	}
};

/* protocol header sizes double-check at compile time */
static_assert(sizeof(rfsp_hdr) == 16, "hdr");

/* ------------------------------------------------------------------ recorder threads */

static std::string url_decode(const std::string &s) {
	std::string o;
	o.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size()) {
			auto hex = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1; };
			int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
			if (hi >= 0 && lo >= 0) { o.push_back(static_cast<char>(hi << 4 | lo)); i += 2; continue; }
		}
		o.push_back(s[i] == '+' ? ' ' : s[i]);
	}
	return o;
}

static std::map<std::string, std::string> parse_query(const std::string &q) {
	std::map<std::string, std::string> kv;
	size_t i = 0;
	while (i < q.size()) {
		size_t amp = q.find('&', i);
		if (amp == std::string::npos) { amp = q.size(); }
		std::string pair = q.substr(i, amp - i);
		size_t eq = pair.find('=');
		if (eq != std::string::npos) { kv[pair.substr(0, eq)] = url_decode(pair.substr(eq + 1)); }
		else if (!pair.empty()) { kv[pair] = ""; }
		i = amp + 1;
	}
	return kv;
}

/* keep the on-disk name to a safe basename ([A-Za-z0-9._-]); default "capture1" */
static std::string rec_sanitize_name(std::string n) {
	std::string o;
	for (char c : n) { if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') { o.push_back(c); } }
	while (!o.empty() && (o.front() == '.' || o.front() == '-')) { o.erase(o.begin()); }
	if (o.empty()) { o = "capture1"; }
	if (o.size() > 64) { o.resize(64); }
	return o;
}

/* one recording session, driven from the arm handshake; owns the file + SigMF sidecar */
static void rec_do_session() {
	uint32_t rate = g_rec.cur_rate.load();
	uint64_t freq = g_rec.cur_freq.load();
	int gain = static_cast<int>(g_rec.cur_gain.load());
	std::string path = g_rec.cur_path;
	int cf32 = g_rec.cf32;
	std::string name = g_rec.name;
	double secs = g_rec.secs;

	auto fail = [&](const char *msg) {
		{ std::lock_guard<std::mutex> lk(g_rec.smtx); g_rec.summary = msg; }
		g_rec.state.store(3);
		logts("record error: %s", msg);
	};

	if (rate == 0) { fail("spectrum not streaming yet"); return; }
	uint64_t want = static_cast<uint64_t>(secs * static_cast<double>(rate));
	if (want == 0) { fail("nothing to record"); return; }

	double bytes_per = cf32 ? 8.0 : 4.0;
	struct statvfs sv;
	if (statvfs(REC_USB_DIR, &sv) != 0) { fail("no usb drive mounted"); return; }
	uint64_t freeb = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
	if (static_cast<double>(want) * bytes_per > static_cast<double>(freeb)) { fail("not enough space on the usb drive"); return; }

	std::string fn = std::string(REC_USB_DIR) + "/" + name + (cf32 ? ".cf32" : ".cs16");
	FILE *f = fopen(fn.c_str(), "wb");
	if (!f) { fail("cannot create file on the usb drive"); return; }

	char dtbuf[32] = "";   /* ISO-8601 UTC capture start for the sidecar */
	{
		struct timespec tw;
		struct tm tmv;
		if (clock_gettime(CLOCK_REALTIME, &tw) == 0 && gmtime_r(&tw.tv_sec, &tmv)) { strftime(dtbuf, sizeof(dtbuf), "%Y-%m-%dT%H:%M:%SZ", &tmv); }
	}

	{ std::lock_guard<std::mutex> lk(g_rec.qmtx); g_rec.filled.clear(); }   /* drop anything stale */
	g_rec.want.store(want);
	g_rec.got.store(0);
	g_rec.dropped.store(0);
	g_rec.stop_req.store(0);
	g_rec.state.store(1);
	g_rec.active.store(true, std::memory_order_release);
	logts("recording %s: %.1f s @ %.2f MSPS, %.3f MHz", fn.c_str(), secs, rate / 1e6, freq / 1e6);

	uint64_t got = 0;
	int peak = 0;
	double psum = 0.0;
	uint64_t pn = 0;
	std::vector<float> fbuf;
	bool stopped = false;
	while (got < want && !g_stop) {
		std::vector<int16_t> b;
		{
			std::unique_lock<std::mutex> lk(g_rec.qmtx);
			g_rec.qcv.wait_for(lk, std::chrono::milliseconds(200), [] { return !g_rec.filled.empty() || g_rec.stop_req.load() || g_stop; });
			if (g_rec.filled.empty()) {
				if (g_rec.stop_req.load() || g_stop) { stopped = true; break; }
				continue;
			}
			b = std::move(g_rec.filled.front());
			g_rec.filled.pop_front();
		}
		size_t er = b.size() / 2;
		if (static_cast<uint64_t>(er) > want - got) { er = static_cast<size_t>(want - got); }
		for (size_t i = 0; i < 2 * er; i += 13) {   /* sparse dBFS stats for the summary */
			int v = b[i] < 0 ? -b[i] : b[i];
			if (v > peak) { peak = v; }
			psum += static_cast<double>(b[i]) * b[i];
			pn++;
		}
		if (cf32) {
			fbuf.resize(2 * er);
			for (size_t i = 0; i < 2 * er; i++) { fbuf[i] = static_cast<float>(b[i]) * (1.0f / 32768.0f); }
			fwrite(fbuf.data(), 8, er, f);
		} else {
			fwrite(b.data(), 4, er, f);
		}
		got += er;
		g_rec.got.store(got);
		{ std::lock_guard<std::mutex> lk(g_rec.qmtx); b.clear(); g_rec.freebufs.push_back(std::move(b)); }
		if (g_rec.stop_req.load()) { stopped = true; break; }
	}
	g_rec.active.store(false, std::memory_order_release);
	fclose(f);

	bool sidecar = false;   /* SigMF Non-Conforming Dataset next to the raw file; failure never fails the capture */
	if (got > 0) {
		std::string base = name + (cf32 ? ".cf32" : ".cs16");
		std::string metafn = std::string(REC_USB_DIR) + "/" + name + ".sigmf-meta";
		struct rtsa_sigmf_meta sm;
		memset(&sm, 0, sizeof(sm));
		sm.datatype = cf32 ? "cf32_le" : "ci16_le";
		sm.dataset = base.c_str();
		sm.sample_rate = rate;
		sm.frequency = static_cast<int64_t>(freq);
		sm.num_channels = 1;
		sm.gain_db = gain;
		sm.rx_path = path.empty() ? nullptr : path.c_str();
		sm.datetime = dtbuf[0] ? dtbuf : nullptr;
		sm.description = "RFNM Lime IQ capture (spectrumd, live view)";
		char meta[1024];
		int mn = rtsa_sigmf_meta_json(&sm, meta, sizeof(meta));
		FILE *mf = mn > 0 ? fopen(metafn.c_str(), "wb") : nullptr;
		if (mf) { sidecar = fwrite(meta, 1, static_cast<size_t>(mn), mf) == static_cast<size_t>(mn); fclose(mf); }
	}

	{
		char sum[256];
		double rms = pn ? sqrt(psum / static_cast<double>(pn)) : 0.0;
		uint64_t drp = g_rec.dropped.load();
		char dmsg[64] = "";
		if (drp) { snprintf(dmsg, sizeof(dmsg), ", %.1fM DROPPED (usb too slow)", static_cast<double>(drp) / 1e6); }
		snprintf(sum, sizeof(sum), "%s: %.1fM samples @ %.2f MS/s, rms %.1f dBFS, peak %.1f dBFS%s%s",
			(name + (cf32 ? ".cf32" : ".cs16")).c_str(), static_cast<double>(got) / 1e6, static_cast<double>(rate) / 1e6,
			20.0 * log10(rms / 32768.0 + 1e-9), 20.0 * log10(static_cast<double>(peak) / 32768.0 + 1e-9),
			sidecar ? " (+SigMF)" : "", dmsg);
		std::lock_guard<std::mutex> lk(g_rec.smtx);
		g_rec.summary = sum;
	}
	/* done if we hit the target or were stopped on request (partial file is still valid); else short */
	g_rec.state.store((got >= want || stopped) ? 2 : 3);
}

static void rec_writer() {
	while (!g_stop) {
		{
			std::unique_lock<std::mutex> lk(g_rec.start_mtx);
			g_rec.start_cv.wait(lk, [] { return g_rec.start_flag || g_stop; });
			g_rec.start_flag = false;
		}
		if (g_stop) { break; }
		rec_do_session();
	}
}

static std::string json_escape(const std::string &s) {
	std::string o;
	for (char c : s) { if (c == '"' || c == '\\') { o.push_back('\\'); } o.push_back(c); }
	return o;
}

static void handle_rec_request(const std::string &path, const std::string &query, int &code, std::string &body) {
	if (path == "/record") {
		auto kv = parse_query(query);
		if (kv.count("stop")) {
			if (g_rec.state.load() == 1) { g_rec.stop_req.store(1); g_rec.qcv.notify_one(); body = "{\"ok\":1,\"stopping\":1}"; }
			else { body = "{\"ok\":1,\"stopping\":0}"; }
			return;
		}
		if (g_rec.state.load() == 1) { code = 409; body = "{\"err\":\"already recording\"}"; return; }
		struct statvfs sv;
		if (statvfs(REC_USB_DIR, &sv) != 0) { code = 400; body = "{\"err\":\"no usb drive mounted\"}"; return; }
		double secs = kv.count("secs") ? strtod(kv["secs"].c_str(), nullptr) : 2.0;
		if (secs <= 0 || secs > 60) { code = 400; body = "{\"err\":\"seconds out of range (1-60)\"}"; return; }
		std::string name = rec_sanitize_name(kv.count("name") ? kv["name"] : "capture1");
		int cf32 = kv.count("fmt") && kv["fmt"] == "cf32";
		/* arm the writer; reserve state=1 now so a second request 409s before the writer runs */
		g_rec.secs = secs;
		g_rec.name = name;
		g_rec.cf32 = cf32;
		g_rec.want.store(0);
		g_rec.got.store(0);
		g_rec.dropped.store(0);
		g_rec.state.store(1);
		{ std::lock_guard<std::mutex> lk(g_rec.start_mtx); g_rec.start_flag = true; }
		g_rec.start_cv.notify_one();
		body = "{\"ok\":1,\"name\":\"" + name + (cf32 ? ".cf32" : ".cs16") + "\"}";
		return;
	}
	if (path == "/recstatus") {
		struct statvfs sv;
		uint64_t freeb = 0;
		int usb = 0;
		if (statvfs(REC_USB_DIR, &sv) == 0) { usb = 1; freeb = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize; }
		std::string sum;
		{ std::lock_guard<std::mutex> lk(g_rec.smtx); sum = g_rec.summary; }
		body = "{\"rec_state\":" + std::to_string(g_rec.state.load()) +
			",\"rec_got\":" + std::to_string(g_rec.got.load()) +
			",\"rec_want\":" + std::to_string(g_rec.want.load()) +
			",\"rec_dropped\":" + std::to_string(g_rec.dropped.load()) +
			",\"cur_freq\":" + std::to_string(g_rec.cur_freq.load()) +
			",\"cur_rate\":" + std::to_string(g_rec.cur_rate.load()) +
			",\"cur_gain\":" + std::to_string(g_rec.cur_gain.load()) +
			",\"usb\":" + std::to_string(usb) +
			",\"free_gb\":" + std::to_string(freeb >> 30) +
			",\"rec_summary\":\"" + json_escape(sum) + "\"}";
		return;
	}
	if (path == "/clients") {
		/* the connection-counted liveness contract (webgui/API.md 4.2): appd stops us only
		 * once this has been 0 across consecutive polls AND the presence heartbeat expired */
		body = "{\"clients\":" + std::to_string(g_stats ? g_stats->clients.load(std::memory_order_relaxed) : 0) +
			",\"rx_parked\":" + ((g_stats && g_stats->rx_parked.load(std::memory_order_relaxed)) ? "1" : "0") + "}";
		return;
	}
	code = 404;
	body = "{\"err\":\"not found\"}";
}

/* minimal loopback HTTP GET server for record control, behind nginx /api/app/rtsa/ (fleet pattern) */
static void rec_http_server(uint16_t port) {
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0) { logts("rec http socket failed"); return; }
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(port);
	if (bind(lfd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) != 0) { logts("rec http bind :%u failed", port); close(lfd); return; }
	listen(lfd, 8);
	logts("rec control http on :%u (nginx proxies /api/app/rtsa)", port);
	while (!g_stop) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(lfd, &rfds);
		struct timeval tv = {1, 0};
		if (select(lfd + 1, &rfds, nullptr, nullptr, &tv) <= 0) { continue; }
		int c = accept(lfd, nullptr, nullptr);
		if (c < 0) { continue; }
		char buf[2048];
		ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
		if (n <= 0) { close(c); continue; }
		buf[n] = 0;
		int code = 200;
		std::string body;
		/* parse "GET /path?query HTTP/1.1" */
		std::string req(buf, static_cast<size_t>(n));
		size_t sp0 = req.find(' ');
		size_t sp1 = sp0 == std::string::npos ? std::string::npos : req.find(' ', sp0 + 1);
		if (sp0 == std::string::npos || sp1 == std::string::npos) { code = 400; body = "{\"err\":\"bad request\"}"; }
		else {
			std::string target = req.substr(sp0 + 1, sp1 - sp0 - 1);
			size_t qm = target.find('?');
			std::string path = qm == std::string::npos ? target : target.substr(0, qm);
			std::string query = qm == std::string::npos ? "" : target.substr(qm + 1);
			handle_rec_request(path, query, code, body);
		}
		std::string hdr = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " ERR") +
			"\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
			"\r\nConnection: close\r\n\r\n";
		send(c, hdr.data(), hdr.size(), MSG_NOSIGNAL);
		send(c, body.data(), body.size(), MSG_NOSIGNAL);
		close(c);
	}
	close(lfd);
}

int main(int argc, char **argv) {
	Cfg cfg;
	g_sweep_prof = getenv("RFNM_SWEEP_PROF") != nullptr;
	for (int i = 1; i < argc; i++) {
		auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
		if (!strcmp(argv[i], "--freq")) { cfg.freq = atof(next()); }
		else if (!strcmp(argv[i], "--gain")) { cfg.gain = atoi(next()); }
		else if (!strcmp(argv[i], "--no-agc")) { cfg.agc = false; }
		else if (!strcmp(argv[i], "--i-only-detector")) { g_iq_det = 0; }   /* pre-reform Q-rail workaround */
		else if (!strcmp(argv[i], "--dc-null")) { g_dc_null = 1; }   /* client-door analog trim (see g_dc_null) */
		else if (!strcmp(argv[i], "--rate")) { cfg.rate = atof(next()); }
		else if (!strcmp(argv[i], "--port")) { cfg.port = static_cast<uint16_t>(atoi(next())); }
		else if (!strcmp(argv[i], "--synthetic")) { cfg.synthetic = true; }
		else if (!strcmp(argv[i], "--synth-rows")) { cfg.synth_rows = atoi(next()); }
		else if (!strcmp(argv[i], "--db-min")) { cfg.db_min = static_cast<float>(atof(next())); }
		else if (!strcmp(argv[i], "--db-max")) { cfg.db_max = static_cast<float>(atof(next())); }
		else if (!strcmp(argv[i], "--path")) { cfg.path = next(); }
		else if (!strcmp(argv[i], "--cpu-fft")) { cfg.cpu_fft = atoi(next()); }
		else if (!strcmp(argv[i], "--no-dc-notch")) { cfg.dc_notch = false; }
		else { fprintf(stderr, "usage: %s [--freq hz] [--gain db] [--no-agc] [--i-only-detector] [--dc-null] [--rate hz] [--port p] [--synthetic] [--synth-rows n] [--db-min v] [--db-max v] [--path embed|a..h] [--cpu-fft n]\n", argv[0]); return 2; }
	}
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

	g_dc_notch = cfg.dc_notch;
#ifndef NO_LIBRFNM
	/* factory-calibration correction for the sweep composer (docs/lime-factory-calib doc;
	 * v1 = zero-median shape, sweep engines only; RFNM_SWEEP_NOCAL=1 disables at run time) */
	{
		const char *ct = getenv("RFNM_SWEEP_CAL");
		int ncal = sweep_cal_load(ct ? ct : "/rfnm/spectrum/calib.tbl");
		if (ncal) {
			logts("factory-cal table: %d rows, serial %s (relative shape; RFNM_SWEEP_NOCAL=1 disables)",
				  ncal, g_sweep_cal.serial[0] ? g_sweep_cal.serial : "?");
		}
	}
	/* #94: per-rate static response curves = THE per-hop correction (there is no live
	 * estimator anymore - see sweep_comp.h). No curve for an armed bw -> honest raw dome. */
	{
		const char *rp = getenv("RFNM_SWEEP_RESP");
		int n = sweep_resp_load(rp ? rp : "/rfnm/spectrum/resp.tbl");
		if (n) { logts("sweep response table: %d curve(s)%s", n, rp ? " (env path)" : ""); }
		else { logts("sweep response table: none (%s) - per-hop dome shows uncorrected", rp ? rp : "/rfnm/spectrum/resp.tbl"); }
	}
#endif
	GpuFFT gpu;
	if (!gpu.init(cfg.db_min, cfg.db_max)) { logts("GPU init failed"); return 1; }
	logts("GC7000UL pipeline ready (batch %d x %d)", BATCH, N_FFT);

	if (cfg.cpu_fft > 2) { cfg.cpu_fft = 2; }
	SlotPool sp(&gpu, cfg.cpu_fft);
	g_slots = &sp;
	RowRing rr;
	Stats st;
	TuneBox tune;
	SweepBox sweepbox;
	ScaleBox scale;
	scale.db_min_c = static_cast<int32_t>(cfg.db_min * 100);
	scale.db_max_c = static_cast<int32_t>(cfg.db_max * 100);
	g_scale = &scale;
	g_cfg = &cfg;
	g_stats = &st;
	static_assert(ROW_RING % BATCH == 0, "ring/batch");

	std::thread t_gpu(gpu_worker, &gpu, &sp, &rr, &st);
	std::vector<std::thread> t_cpu;
	for (int i = 0; i < cfg.cpu_fft; i++) { t_cpu.emplace_back(cpu_fft_worker, i, &cfg, &sp, &rr, &st); }
	if (cfg.cpu_fft) { logts("hybrid FFT: GPU + %d A53 pffft workers", cfg.cpu_fft); }
	std::thread t_src;
	if (cfg.synthetic) {
		t_src = std::thread(synth_source, &cfg, &sp, &rr, &st);
		logts("synthetic source at ~%d rows/s", cfg.synth_rows);
	} else {
#ifndef NO_LIBRFNM
		t_src = std::thread(rx_source, &cfg, &tune, &sweepbox, &sp, &rr, &st);
#else
		logts("built with NO_LIBRFNM and --synthetic not given");
		return 1;
#endif
	}

	WsServer ws{&cfg, &rr, &st, &tune, &scale, &sweepbox};
	if (!ws.init()) { return 1; }
	logts("ws on :%u (nginx proxies /ws)", cfg.port);

	std::thread t_rec(rec_writer);
	std::thread t_rechttp(rec_http_server, static_cast<uint16_t>(7687));

	ws.run();

	g_stop = 1;
	g_rec.start_cv.notify_all();   /* wake the recorder threads so they see g_stop and exit */
	g_rec.qcv.notify_all();
	t_gpu.join();
	for (auto &t : t_cpu) { t.join(); }
	if (t_src.joinable()) { t_src.join(); }
	t_rec.join();
	t_rechttp.join();
	logts("bye");
	return 0;
}
