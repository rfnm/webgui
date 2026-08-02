// ismmond — real-time 2.4 GHz ISM monitor daemon for the RFNM Lime web GUI (RX only).
//   Owns TRANSPORT_LOCAL RX @ 100 MSPS / 2441 MHz (whole ISM band; drop-tolerant, sampling monitor
//   — see docs/ism-monitor). Reuses spectrumd's GPU FFT engine (fft_engine.h) to reduce the live
//   stream: the RX thread drains samples into a history ring and hands FFT batches to a GPU thread
//   (freshest-wins, never blocking RX); the GPU FFTs what it can (~1/4 of the stream — ample to
//   discover repeating advertisers) and a per-channel Detector thresholds the rows to find bursts;
//   each burst's raw IQ is pulled from the ring (which holds ~92% of the stream), channelized, and
//   demodulated (BLE LE 1M advertising). Decoded headers -> Store -> /ismapi/ HTTP behind nginx.
//   Rate is set ONCE at startup and never reclocked (startup check warns on the 1/3-rate DCS state).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

#include "channelizer.h"
#include "detector.h"
#include "fft_engine.h"
#include "gfsk_rx.h"
#include "httpd.h"
#include "store.h"
#include "wifi.h"
#include "wifi_dsss.h"

#ifdef WITH_RFNM
#include "capture.h"
#endif

using namespace ismmon;

static std::atomic<bool> g_stop{false};
static void on_sig(int) { g_stop = true; }

struct Cfg {
	double freq = 2441e6;
	double rate = 100e6;
	int gain = 30;
	int agc = 0;                             // 1: device hardware AGC on (--gain stays the starting gain)
	std::string antenna = "embed";
	int port = 7688;
	float db_min = -92.0f, db_max = 98.0f;   // GPU compute-domain dB window (spectrumd's values)
	float margin = 18.0f;                    // detector trigger margin (u8 units above floor)
	double chan_rate = 2.0e6;                // per-channel demod rate (decim 50 @ 100 MSPS -> sps 2.0)
	bool wifi = false;                       // enable the 802.11a/g Wi-Fi scan thread (opt-in: --wifi)
};

// CS16 history ring: RX appends here so a detected burst's raw IQ can be pulled back for demod.
// write_abs is atomic so the GPU-thread reader sees a consistent, published bound; the read region
// (a burst ~10-20 ms old) is far behind the write head, so it is never being overwritten mid-read.
struct IqRing {
	std::vector<int16_t> buf;
	uint64_t cap;
	std::atomic<uint64_t> write_abs{0};
	explicit IqRing(uint64_t c) : buf((size_t)c * 2, 0), cap(c) {}
	void push(const int16_t* iq, size_t n) {
		uint64_t wa = write_abs.load(std::memory_order_relaxed);
		uint64_t pos = wa % cap;
		size_t first = (n < cap - pos) ? n : (size_t)(cap - pos);
		memcpy(&buf[2 * pos], iq, first * 2 * sizeof(int16_t));
		if(n > first) { memcpy(&buf[0], iq + 2 * first, (n - first) * 2 * sizeof(int16_t)); }
		write_abs.store(wa + n, std::memory_order_release);
	}
	bool extract(uint64_t a, uint64_t b, std::vector<cf>& out) {
		uint64_t wa = write_abs.load(std::memory_order_acquire);
		if(b <= a || b > wa || a + cap <= wa) { return false; }
		out.resize(b - a);
		// CS16 int16 -> complex float, NEON over contiguous ring runs (split at the wrap). cf is [re,im]
		// contiguous, so this converts the flat interleaved stream. Bit-exact vs the scalar fallback
		// (validated aarch64/qemu). Runs per demod-window extract (the Wi-Fi scan pulls ~500k/scan).
		float* od = reinterpret_cast<float*>(out.data());
		uint64_t i = a;
		while(i < b) {
			uint64_t idx = i % cap;
			uint64_t run = (b - i < cap - idx) ? (b - i) : (cap - idx);   // contiguous samples before wrap
			const int16_t* src = &buf[2 * idx];
			float* dst = od + 2 * (i - a);
			size_t nf = (size_t)run * 2, k = 0;
#if defined(__aarch64__) || defined(__ARM_NEON)
			const float32x4_t sc = vdupq_n_f32(1.0f / 32768.0f);
			for(; k + 8 <= nf; k += 8) {
				int16x8_t v = vld1q_s16(src + k);
				vst1q_f32(dst + k, vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(v))), sc));
				vst1q_f32(dst + k + 4, vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(v))), sc));
			}
#endif
			for(; k < nf; k++) { dst[k] = src[k] * (1.0f / 32768.0f); }
			i += run;
		}
		return true;
	}
};

struct Engine {
	Cfg cfg;
	GpuFFT fft;
	IqRing ring;
	Store store;
	Detector det;
	bool synchronous;
	std::atomic<uint64_t> rows_fft{0}, bursts{0}, decoded{0}, dropped_batches{0}, found_sync{0};
	std::atomic<uint64_t> wifi_frames{0}, wifi_aps{0};
	std::map<std::string, int> wifi_sta_seen;   // data-frame transmitters; confirmed as devices on 2nd sighting
	int sync_logs = 0;
	std::thread wifi_thread;

	// two GPU input slots (fft.inMap[0/1]); RX producer, gpu thread consumer, freshest-wins.
	std::atomic<int> slot_state[2];    // 0=FREE, 1=FILLED
	uint64_t slot_abs[2] = {0, 0};
	std::mutex mtx;
	std::condition_variable cv;
	std::atomic<bool> stop{false};
	std::thread gpu_thread;

	int rx_slot = -1;
	size_t fill = 0;
	uint64_t cur_batch_abs = 0, global_row = 0;
	std::vector<uint64_t> abs_of_row;
	std::vector<uint8_t> rows;

	Engine(const Cfg& c, std::vector<ChannelBand> bands, uint64_t ring_cap, bool sync)
		: cfg(c), ring(ring_cap), det(std::move(bands), c.margin, 2, 96), synchronous(sync),
		  abs_of_row(2048, 0), rows((size_t)BATCH * N_FFT) {
		slot_state[0].store(0);
		slot_state[1].store(0);
	}

	void start() {
		if(!synchronous) {
			gpu_thread = std::thread([this] { gpu_loop(); });
			if(cfg.wifi) { wifi_thread = std::thread([this] { wifi_loop(); }); }
		}
	}
	void shutdown() {
		stop.store(true);
		cv.notify_all();
		if(gpu_thread.joinable()) { gpu_thread.join(); }
		if(wifi_thread.joinable()) { wifi_thread.join(); }
	}

	// Wi-Fi: periodically sample each 2.4 GHz Wi-Fi channel from the ring, channelize to 20 MSPS, and
	// scan with the 802.11a/g OFDM decoder. Rate-limited so the busy Wi-Fi band doesn't overload the
	// A53s. Presence = L-SIG-decoded frame count; SSIDs from FCS-valid (legacy) management frames.
	void wifi_loop() {
		// sweep all 2.4 GHz channels 1..13 (2412..2472) - APs are not only on 1/6/11. One channel per
		// scan (rotate); over ~5 s the whole band is covered, and the AP table accumulates over minutes.
		struct WCh { int ch; double f; };
		std::vector<WCh> wch;
		for(int i = 0; i < 13; i++) { wch.push_back({ i + 1, 2412e6 + 5e6 * i }); }
		{ cpu_set_t s; CPU_ZERO(&s); CPU_SET(1, &s); pthread_setaffinity_np(pthread_self(), sizeof(s), &s); }
		size_t rot = 0;
		while(!stop.load()) {
			for(int i = 0; i < 16 && !stop.load(); i++) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); }   // ~400 ms
			if(stop.load()) { break; }
			uint64_t wa = ring.write_abs.load();
			uint64_t span = 500000;   // ~5 ms @ 100 MSPS (-> 125k @ 25 MSPS; covers a 3.5 ms beacon window)
			if(wa < span + 2000) { continue; }
			std::vector<cf> raw;
			if(!ring.extract(wa - span - 1000, wa - 1000, raw)) { continue; }
			auto& w = wch[rot];
			rot = (rot + 1) % wch.size();   // one channel per scan (rotate) - keeps the busy Wi-Fi band off the RX cores
			// 802.11b DSSS is the PHY these APs beacon at (real SSIDs decode here). Channelize to
			// 25 MSPS and slide a couple of windows so a beacon anywhere in the span is caught.
			ddc dc(cfg.rate, DSSS_FS, w.f - cfg.freq, 0.44);
			std::vector<cf> chn;
			dc.process(raw.data(), raw.size(), chn);
			size_t W = (size_t)(3.5e-3 * DSSS_FS);   // 3.5 ms window (beacons run ~2-3 ms)
			size_t wstep = (size_t)(0.9e-3 * DSSS_FS);   // fine slide so a beacon lands "first" in some window
			for(size_t p = 0; p + W <= chn.size(); p += wstep) {
				wifi_packet pk;
				if(dsss_decode_burst(&chn[p], W, w.ch, pk)) {
					if(pk.sig_ok) { wifi_frames++; }
					// SSID/BSSID come from a header-CRC-valid frame with a well-formed printable SSID -
					// no full-frame FCS needed (that would drop every AP but the strongest few).
					if(pk.has_addr && !pk.ssid.empty()) { wifi_aps++; store.add_wifi(pk); }
					else if(pk.has_addr && pk.frame_type == 0x20) {
						// data-frame transmitter = a client/AP device (unencrypted MAC header). Confirm on
						// the 2nd sighting to guard against a one-off bit error, then surface it.
						if(++wifi_sta_seen[wifi_mac_string(pk.bssid)] == 2) { wifi_aps++; store.add_wifi(pk); }
					}
				}
			}
		}
	}

	// FFT one filled slot -> detector; caller owns slot lifecycle.
	void process_slot(int s) {
		if(!fft.run(s, rows.data(), BATCH)) { return; }
		rows_fft += BATCH;
		uint64_t base = global_row;
		global_row += BATCH;
		for(int w = 0; w < BATCH; w++) { abs_of_row[(base + w) % abs_of_row.size()] = slot_abs[s] + (uint64_t)w * N_FFT; }
		auto cb = [this](const BurstEvent& e) { on_burst(e); };
		for(int w = 0; w < BATCH; w++) { det.feed_row(base + w, rows.data() + (size_t)w * N_FFT, N_FFT, cb); }
		det.flush(cb);   // per-batch: bursts never span the (dropped) inter-batch gap
	}

	void gpu_loop() {
		while(!stop.load()) {
			int s = -1;
			{
				std::unique_lock<std::mutex> lk(mtx);
				cv.wait(lk, [this] { return stop.load() || slot_state[0].load() == 1 || slot_state[1].load() == 1; });
				if(stop.load()) { break; }
				s = (slot_state[0].load() == 1) ? 0 : 1;
			}
			process_slot(s);
			slot_state[s].store(0);
		}
	}

	void on_burst(const BurstEvent& e) {
		bursts++;
		uint64_t a = abs_of_row[e.row_start % abs_of_row.size()];
		uint64_t b = abs_of_row[e.row_end % abs_of_row.size()] + N_FFT;
		uint64_t marg = 4 * N_FFT;
		a = (a > marg) ? a - marg : 0;
		b = b + marg;
		if(b - a > (1u << 19)) { return; }
		std::vector<cf> span;
		if(!ring.extract(a, b, span)) { return; }
		double off = ble_adv_freq(e.channel_id) - cfg.freq;
		ddc dc(cfg.rate, cfg.chan_rate, off);
		std::vector<cf> bb;
		dc.process(span.data(), span.size(), bb);
		ble_packet pkt;
		double sps = cfg.chan_rate / BLE_SYM_RATE;
		bool found = demod_ble_burst(bb.data(), bb.size(), e.channel_id, sps, pkt, 0.55);
		if(!found) { return; }
		found_sync++;   // the BLE access address correlated -> a real BLE burst
		if(pkt.crc_ok) {
			decoded++;
			store.add(pkt);
			printf("ismmond: ch%d %-14s %s rssi %.0f cfo %+.0fkHz len %zu name='%s'\n",
			       e.channel_id, ble_pdu_type_name(pkt.adv.pdu_type),
			       pkt.adv.has_adva ? ble_adva_string(pkt.adv.adva).c_str() : "?",
			       pkt.rssi_db, pkt.cfo_hz / 1e3, pkt.pdu.size(), pkt.adv.name.c_str());
			fflush(stdout);
		} else if(sync_logs < 30) {
			// AA matched but CRC failed: dump evidence to confirm/fix the CRC-24/whitening convention
			sync_logs++;
			printf("ismmond: SYNC-NOCRC ch%d corr %.2f rssi %.0f cfo %+.0fkHz crc_rx=%06x calc=%06x pdu[%zu]=%s\n",
			       e.channel_id, pkt.sync_corr, pkt.rssi_db, pkt.cfo_hz / 1e3, pkt.crc_rx, pkt.crc_calc,
			       pkt.pdu.size(), hexstr(pkt.pdu).c_str());
			fflush(stdout);
		}
	}

	// RX callback: ring + fill FFT batches. Live: hand full batches to the gpu thread, drop when it
	// is behind (samples stay in the ring). Synchronous (selftest): process each full batch inline.
	void feed(const int16_t* iq, size_t n) {
		ring.push(iq, n);
		size_t off = 0;
		while(off < n) {
			if(rx_slot < 0) {
				if(slot_state[0].load() == 0) { rx_slot = 0; }
				else if(slot_state[1].load() == 0) { rx_slot = 1; }
				else { dropped_batches++; break; }
				fill = 0;
				cur_batch_abs = ring.write_abs.load() - n + off;
			}
			size_t take = n - off, room = (size_t)BATCH * N_FFT - fill;
			if(take > room) { take = room; }
			memcpy(fft.inMap[rx_slot] + 2 * fill, iq + 2 * off, take * 2 * sizeof(int16_t));
			fill += take;
			off += take;
			if(fill == (size_t)BATCH * N_FFT) {
				slot_abs[rx_slot] = cur_batch_abs;
				if(synchronous) {
					process_slot(rx_slot);
					slot_state[rx_slot].store(0);
				} else {
					slot_state[rx_slot].store(1);
					cv.notify_one();
				}
				rx_slot = -1;
			}
		}
	}
};

static std::vector<ChannelBand> ble_bands(const Cfg& c) {
	auto band = [&](int chidx) {
		double o = ble_adv_freq(chidx) - c.freq;
		int cb = N_FFT / 2 + (int)std::lround(o / c.rate * N_FFT);
		int half = (int)std::lround(1.1e6 / c.rate * N_FFT);
		return ChannelBand{ chidx, cb - half, cb + half };
	};
	return { band(37), band(38), band(39) };
}

// on-board acceptance: synthesize a wideband CS16 buffer with a BLE packet at ch38, push it through
// the real GPU-FFT -> detector -> demod pipeline (synchronous, no drops), assert a decode.
static int run_selftest(Cfg cfg) {
	Engine eng(cfg, ble_bands(cfg), 1u << 22, /*sync*/ true);
	if(!eng.fft.init(cfg.db_min, cfg.db_max)) {
		fprintf(stderr, "ismmond --selftest: GPU init failed (needs the board GC7000UL)\n");
		return 1;
	}
	uint8_t adva[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };
	std::vector<uint8_t> pl = { adva[0],adva[1],adva[2],adva[3],adva[4],adva[5], 2,0x01,0x06, 5,0x09,'L','i','m','e' };
	std::vector<uint8_t> pdu = { 0x00, (uint8_t)pl.size() };
	pdu.insert(pdu.end(), pl.begin(), pl.end());
	bits stream = ble_build_adv_stream(pdu, 38);
	std::vector<cf> sig = gfsk_modulate(stream, cfg.rate);
	double doff = ble_adv_freq(38) - cfg.freq, dph = 2.0 * M_PI * doff / cfg.rate;
	size_t lead = 3 * BATCH * N_FFT, total = lead + sig.size() + BATCH * N_FFT;
	std::vector<int16_t> cs16(total * 2);
	unsigned rng = 12345;
	auto nrand = [&]() { rng = rng * 1664525u + 1013904223u; return ((int)(rng >> 9) - 4194304) / 4194304.0f * 0.03f; };
	for(size_t i = 0; i < total; i++) {
		cf v(nrand(), nrand());
		if(i >= lead && i - lead < sig.size()) {
			double ph = dph * (double)(i - lead);
			v += sig[i - lead] * cf((float)cos(ph), (float)sin(ph));
		}
		int re = (int)(v.real() * 32768.0f), im = (int)(v.imag() * 32768.0f);
		cs16[2 * i] = (int16_t)(re < -32768 ? -32768 : re > 32767 ? 32767 : re);
		cs16[2 * i + 1] = (int16_t)(im < -32768 ? -32768 : im > 32767 ? 32767 : im);
	}
	size_t chunk = 100000;
	for(size_t o = 0; o < total; o += chunk) {
		size_t nn = (o + chunk <= total) ? chunk : total - o;
		eng.feed(&cs16[2 * o], nn);
	}
	printf("ismmond --selftest: rows_fft=%llu bursts=%llu decoded=%llu devices=%zu\n",
	       (unsigned long long)eng.rows_fft.load(), (unsigned long long)eng.bursts.load(),
	       (unsigned long long)eng.decoded.load(), eng.store.device_count());
	bool ok = eng.decoded.load() >= 1 && eng.store.device_count() >= 1;
	printf("ismmond --selftest: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}

static void usage() {
	fprintf(stderr, "usage: ismmond [--freq HZ] [--rate HZ] [--gain dB] [--agc 0|1] [--antenna name] [--port n]\n"
	                "               [--dbmin f] [--dbmax f] [--margin u8] [--wifi] [--selftest]\n");
}

int main(int argc, char** argv) {
	Cfg cfg;
	bool selftest = false;
	for(int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if(a == "--selftest") { selftest = true; }
		else if(a == "--freq" && i + 1 < argc) { cfg.freq = atof(argv[++i]); }
		else if(a == "--rate" && i + 1 < argc) { cfg.rate = atof(argv[++i]); }
		else if(a == "--gain" && i + 1 < argc) { cfg.gain = atoi(argv[++i]); }
		else if(a == "--agc" && i + 1 < argc) { cfg.agc = atoi(argv[++i]); }
		else if(a == "--antenna" && i + 1 < argc) { cfg.antenna = argv[++i]; }
		else if(a == "--port" && i + 1 < argc) { cfg.port = atoi(argv[++i]); }
		else if(a == "--dbmin" && i + 1 < argc) { cfg.db_min = atof(argv[++i]); }
		else if(a == "--dbmax" && i + 1 < argc) { cfg.db_max = atof(argv[++i]); }
		else if(a == "--margin" && i + 1 < argc) { cfg.margin = atof(argv[++i]); }
		else if(a == "--wifi") { cfg.wifi = true; }
		else { usage(); return 2; }
	}

	if(selftest) { return run_selftest(cfg); }

#ifndef WITH_RFNM
	fprintf(stderr, "ismmond: built without librfnm (WITH_RFNM=0); only --selftest works (needs the GPU)\n");
	return 2;
#else
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

	Engine eng(cfg, ble_bands(cfg), 1u << 23, /*sync*/ false);   // ~8.4M complex ring (~84 ms @100 MSPS)
	if(!eng.fft.init(cfg.db_min, cfg.db_max)) {
		fprintf(stderr, "ismmond: GPU FFT init failed (check libEGL symlink / GC7000UL)\n");
		return 1;
	}
	eng.start();

	auto t_start = std::chrono::steady_clock::now();
	std::atomic<uint64_t> samples{0};

	httpd server(cfg.port, [&](const std::string& path_in) -> std::string {
		std::string p = path_in;
		if(p.rfind("/ismapi", 0) == 0) { p = p.substr(7); }
		if(p == "/devices") { return eng.store.devices_json(); }
		if(p == "/packets") { return eng.store.packets_json(); }
		if(p == "/status" || p == "/" || p.empty()) {
			double up = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
			double eff = up > 0 ? samples.load() / up / 1e6 : 0;
			char b[600];
			snprintf(b, sizeof(b),
			         "{\"freq\":%.0f,\"rate\":%.0f,\"effective_msps\":%.2f,\"coverage_pct\":%.1f,"
			         "\"fft_coverage_pct\":%.1f,\"uptime\":%.1f,\"rows_fft\":%llu,\"bursts\":%llu,"
			         "\"found_sync\":%llu,\"decoded\":%llu,\"dropped_batches\":%llu,\"devices\":%zu,"
			         "\"total\":%llu,\"wifi_frames\":%llu,\"wifi_aps\":%llu,\"gpu_ms\":%.2f}",
			         cfg.freq, cfg.rate, eff, cfg.rate > 0 ? 100.0 * eff * 1e6 / cfg.rate : 0,
			         eff > 0 ? 100.0 * (eng.rows_fft.load() * (double)N_FFT) / (eff * 1e6 * up) : 0, up,
			         (unsigned long long)eng.rows_fft.load(), (unsigned long long)eng.bursts.load(),
			         (unsigned long long)eng.found_sync.load(), (unsigned long long)eng.decoded.load(),
			         (unsigned long long)eng.dropped_batches.load(),
			         eng.store.device_count(), (unsigned long long)eng.store.total(),
			         (unsigned long long)eng.wifi_frames.load(), (unsigned long long)eng.wifi_aps.load(), eng.fft.avg_batch_ms);
			return b;
		}
		return "";
	});
	if(!server.ok()) { fprintf(stderr, "ismmond: http bind failed on :%d\n", cfg.port); eng.shutdown(); return 1; }

	printf("ismmond: freq=%.3f MHz rate=%.3f MSps gain=%d agc=%d antenna=%s http=:%d — watching BLE ch37/38/39\n",
	       cfg.freq / 1e6, cfg.rate / 1e6, cfg.gain, cfg.agc, cfg.antenna.c_str(), cfg.port);
	fflush(stdout);

	std::thread checker([&] {
		std::this_thread::sleep_for(std::chrono::seconds(4));
		double up = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
		double eff = up > 0 ? samples.load() / up / 1e6 : 0;
		if(samples.load() > 0 && eff < 0.6 * cfg.rate / 1e6) {
			fprintf(stderr, "ismmond: WARNING effective rate %.1f MSps << requested %.1f — DCS landed in the "
			                "1/3 state (reclock churn). Full band needs a power-cycle.\n", eff, cfg.rate / 1e6);
		} else {
			fprintf(stderr, "ismmond: rate OK, effective %.1f MSps (%.0f%% coverage), gpu %.1f ms/batch\n",
			        eff, 100.0 * eff * 1e6 / cfg.rate, eng.fft.avg_batch_ms);
		}
	});
	checker.detach();

	bool ok = run_capture(cfg.freq, cfg.rate, cfg.gain, cfg.agc, cfg.antenna, g_stop, samples,
	                      [&](const int16_t* iq, size_t n) { eng.feed(iq, n); });
	g_stop = true;
	eng.shutdown();
	printf("ismmond: exiting (%s), %llu samples, %llu decoded\n", ok ? "clean" : "capture failed",
	       (unsigned long long)samples.load(), (unsigned long long)eng.decoded.load());
	return ok ? 0 : 1;
#endif
}
