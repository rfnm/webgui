// ismmon Phase-2 wideband pipeline selftest (host, no hardware / no GPU). Builds a 76.8 MHz-wide
// scene with a BLE advertising packet at ch38's offset (-15 MHz from 2441) buried in noise, runs a
// small STFT -> u8-dB rows -> Detector (the real burst detector) -> maps the burst rows to a raw-IQ
// span -> ddc channelize (decim 32 -> 2.4 MHz) -> demod_ble_burst. Proves detect -> channelize ->
// decode, the exact chain ismmond runs (on-board the STFT is the shared GPU FFT engine).
#include "ble.h"
#include "channelizer.h"
#include "detector.h"
#include "gfsk_rx.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace ismmon;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { if(cond) { g_pass++; } else { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while(0)

// compact iterative radix-2 FFT (test-only; the daemon uses the shared GPU/pffft engine)
static void fft(std::vector<cf>& a) {
	int n = (int)a.size();
	for(int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for(; j & bit; bit >>= 1) { j ^= bit; }
		j ^= bit;
		if(i < j) { std::swap(a[i], a[j]); }
	}
	for(int len = 2; len <= n; len <<= 1) {
		double ang = -2.0 * M_PI / len;
		cf wl((float)cos(ang), (float)sin(ang));
		for(int i = 0; i < n; i += len) {
			cf w(1, 0);
			for(int k = 0; k < len / 2; k++) {
				cf u = a[i + k], v = a[i + k + len / 2] * w;
				a[i + k] = u + v;
				a[i + k + len / 2] = u - v;
				w *= wl;
			}
		}
	}
}

int main() {
	const double FS = 76.8e6;
	const double CENTER = 2441e6;
	const int NFFT = 1024, HOP = 512;
	std::mt19937 rng(2026);

	// --- build the advertising packet, modulated at the wideband rate, shifted to ch38 (-15 MHz) ---
	uint8_t adva[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xCF };
	std::vector<uint8_t> pl = { adva[0],adva[1],adva[2],adva[3],adva[4],adva[5],
	                            2, 0x01, 0x06, 5, 0x09, 'L','i','m','e' };
	std::vector<uint8_t> pdu = { 0x00, (uint8_t)pl.size() };
	pdu.insert(pdu.end(), pl.begin(), pl.end());
	bits stream = ble_build_adv_stream(pdu, 38);
	std::vector<cf> pkt = gfsk_modulate(stream, FS);           // sps = 76.8
	double off = ble_adv_freq(38) - CENTER;                    // -15e6
	double dph = 2.0 * M_PI * off / FS;
	for(size_t i = 0; i < pkt.size(); i++) {
		pkt[i] *= cf((float)cos(dph * i), (float)sin(dph * i));
	}

	// --- wideband scene: lead + packet + trail, + AWGN (~25 dB) ---
	int lead = 6000, trail = 6000;
	std::vector<cf> scene(lead + pkt.size() + trail, cf(0, 0));
	for(size_t i = 0; i < pkt.size(); i++) { scene[lead + i] = pkt[i]; }
	double sigma = std::sqrt(1.0 / (2.0 * std::pow(10.0, 25.0 / 10.0)));
	std::normal_distribution<float> nd(0.0f, (float)sigma);
	for(auto& x : scene) { x += cf(nd(rng), nd(rng)); }

	// --- STFT -> u8-dB rows (fftshifted) ---
	std::vector<float> win(NFFT);
	for(int i = 0; i < NFFT; i++) { win[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (NFFT - 1))); }
	int nrows = (int)((scene.size() - NFFT) / HOP);
	std::vector<std::vector<uint8_t>> rows(nrows, std::vector<uint8_t>(NFFT));
	const float dbmin = -20.0f, dbscale = 4.0f;
	for(int r = 0; r < nrows; r++) {
		std::vector<cf> f(NFFT);
		for(int i = 0; i < NFFT; i++) { f[i] = scene[r * HOP + i] * win[i]; }
		fft(f);
		for(int k = 0; k < NFFT; k++) {
			cf z = f[(k + NFFT / 2) % NFFT];   // fftshift: row bin k -> freq (k-N/2)/N*fs
			float mag2 = z.real() * z.real() + z.imag() * z.imag() + 1e-12f;
			float db = 10.0f * log10f(mag2);
			float v = (db - dbmin) * dbscale;
			rows[r][k] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
		}
	}

	// --- channel bin map. At 76.8/2441 only ch38 is in-band (Phase 0: ch37=-39 MHz, ch39=+39 MHz
	// fall outside the +-38.4 MHz edge). Monitor ch38 + a QUIET in-band control at +10 MHz to prove
	// the detector fires on the packet and NOT on empty in-band spectrum. ---
	auto band_at = [&](int id, double o) {
		int center_bin = NFFT / 2 + (int)std::lround(o / FS * NFFT);
		int half = (int)std::lround(1.0e6 / FS * NFFT);   // +-1 MHz
		return ChannelBand{ id, center_bin - half, center_bin + half };
	};
	std::vector<ChannelBand> chans = { band_at(38, ble_adv_freq(38) - CENTER), band_at(999, 10e6) };

	// --- run the real detector over the rows ---
	std::vector<BurstEvent> bursts;
	Detector det(chans, /*margin*/ 20.0f, /*hang*/ 2, /*max_rows*/ 200);
	for(int r = 0; r < nrows; r++) {
		det.feed_row((uint64_t)r, rows[r].data(), NFFT,
		             [&](const BurstEvent& e) { bursts.push_back(e); });
	}

	// exactly one burst, on ch38, roughly where we placed the packet
	int ch38_bursts = 0;
	const BurstEvent* be = nullptr;
	for(auto& e : bursts) { if(e.channel_id == 38) { ch38_bursts++; be = &e; } }
	CHECK(ch38_bursts == 1, "expected 1 ch38 burst, got %d (total %zu)", ch38_bursts, bursts.size());
	for(auto& e : bursts) { CHECK(e.channel_id == 38, "spurious burst on ch%d", e.channel_id); }

	if(be) {
		uint64_t s0 = be->row_start * HOP;
		uint64_t s1 = (be->row_end + 1) * HOP + NFFT;
		double pkt_lo = lead, pkt_hi = lead + pkt.size();
		CHECK((double)s0 <= pkt_lo + 4 * HOP && (double)s1 >= pkt_hi - 4 * HOP,
		      "burst span [%llu,%llu] misses packet [%d,%zu]", (unsigned long long)s0,
		      (unsigned long long)s1, lead, lead + pkt.size());

		// --- channelize the detected span to ch38 baseband, then demod ---
		uint64_t m = 512;   // margin for FIR warm-up
		uint64_t a = s0 > m ? s0 - m : 0;
		uint64_t b = s1 + m < scene.size() ? s1 + m : scene.size();
		ddc chan(FS, 2.4e6, off);
		std::vector<cf> bb;
		chan.process(&scene[a], b - a, bb);
		ble_packet pkt_out;
		bool found = demod_ble_burst(bb.data(), bb.size(), 38, 2.4, pkt_out);
		CHECK(found && pkt_out.crc_ok, "wideband decode failed (found %d corr %.2f crc %d)",
		      found, pkt_out.sync_corr, pkt_out.crc_ok);
		CHECK(pkt_out.pdu == pdu, "wideband PDU mismatch");
		CHECK(pkt_out.adv.name == "Lime", "wideband name mismatch ('%s')", pkt_out.adv.name.c_str());
		printf("  detected ch38 burst rows %llu..%llu peak %.0f floor %.0f -> decoded '%s' %s cfo %.0f kHz\n",
		       (unsigned long long)be->row_start, (unsigned long long)be->row_end, be->peak_db, be->floor_db,
		       pkt_out.adv.name.c_str(), ble_adva_string(pkt_out.adv.adva).c_str(), pkt_out.cfo_hz / 1e3);
	}

	printf("\nismmon wb_selftest: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
