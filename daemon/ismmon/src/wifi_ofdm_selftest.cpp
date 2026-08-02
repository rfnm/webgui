// ismmon 802.11a/g OFDM selftest - ALL 8 legacy rates (BPSK/QPSK/16QAM/64QAM + puncturing) and the
// HT (11n) path. Round-trips a beacon through wifi_ofdm_modulate at each rate, adds lead/CFO/AWGN, and
// asserts the decoder recovers the SSID/BSSID. Self-consistent gate for the extended-rate + HT work.
#include "wifi.h"
#include "wifi_ht.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace ismmon;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if(c) { g_pass++; } else { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while(0)

static std::vector<cf> make_burst(const std::vector<cf>& sig, double cfo_hz, double snr_db, int lead, std::mt19937& rng) {
	std::vector<cf> out(lead + sig.size() + 200, cf(0, 0));
	double dph = 2.0 * M_PI * cfo_hz / WIFI_FS;
	for(size_t i = 0; i < sig.size(); i++) {
		double ph = dph * (double)i;
		out[lead + i] = sig[i] * cf((float)cos(ph), (float)sin(ph));
	}
	if(snr_db < 200) {
		double sp = 0; for(auto& x : sig) { sp += std::norm(x); } sp /= sig.size();
		double sigma = std::sqrt(sp / (2.0 * std::pow(10.0, snr_db / 10.0)));
		std::normal_distribution<float> nd(0.0f, (float)sigma);
		for(auto& x : out) { x += cf(nd(rng), nd(rng)); }
	}
	return out;
}

int main() {
	std::mt19937 rng(7);
	uint8_t bssid[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	std::string ssid = "RFNM-OFDM-AP";
	std::vector<uint8_t> psdu = wifi_build_beacon(bssid, ssid);
	printf("  beacon PSDU %zu bytes ssid='%s'\n", psdu.size(), ssid.c_str());

	int rates[8] = { 6, 9, 12, 18, 24, 36, 48, 54 };
	printf("  -- all legacy rates, clean --\n");
	for(int r : rates) {
		std::vector<cf> sig = wifi_ofdm_modulate(psdu, r, 0x5d);
		std::vector<cf> b = make_burst(sig, 0, 200, 200, rng);
		wifi_packet pk;
		bool ok = wifi_decode_burst(b.data(), b.size(), 6, pk);
		CHECK(ok && pk.sig_ok && pk.rate_mbps == r, "rate %d: SIGNAL (ok %d rate %d)", r, ok, pk.rate_mbps);
		CHECK(pk.mac_ok, "rate %d: MAC/FCS not decoded (clean)", r);
		CHECK(pk.ssid == ssid, "rate %d: ssid '%s'", r, pk.ssid.c_str());
		if(pk.mac_ok) { printf("     %2d Mbps: '%s' bssid %s\n", r, pk.ssid.c_str(), wifi_mac_string(pk.bssid).c_str()); }
	}

	printf("  -- CFO + noise (rate 6/12/24, cfo +50k, 22 dB) --\n");
	for(int r : { 6, 12, 24 }) {
		std::vector<cf> sig = wifi_ofdm_modulate(psdu, r, 0x11);
		std::vector<cf> b = make_burst(sig, 50e3, 22, 200, rng);
		wifi_packet pk;
		bool ok = wifi_decode_burst(b.data(), b.size(), 6, pk);
		CHECK(ok && pk.ssid == ssid, "rate %d cfo/noise: ssid='%s' mac_ok %d", r, pk.ssid.c_str(), pk.mac_ok);
	}

	// ---- HT (11n) HT-SIG: detection + MCS/length/CRC-8 across all single-stream MCS ----
	printf("  -- HT (11n) HT-SIG, MCS 0..7 --\n");
	for(int mcs = 0; mcs <= 7; mcs++) {
		std::vector<cf> sig = wifi_ht_modulate(psdu, mcs, 0x33);
		std::vector<cf> b = make_burst(sig, 0, 200, 200, rng);
		wifi_packet pk;
		bool ok = wifi_decode_burst(b.data(), b.size(), 6, pk);
		CHECK(ok && pk.is_ht, "HT MCS%d: not detected as HT (ok %d is_ht %d)", mcs, ok, pk.is_ht);
		CHECK(pk.htsig_ok, "HT MCS%d: HT-SIG CRC-8 invalid", mcs);
		CHECK(pk.mcs == mcs, "HT MCS%d: decoded MCS %d", mcs, pk.mcs);
		CHECK(pk.ht_length == (int)psdu.size(), "HT MCS%d: HT-length %d (want %zu)", mcs, pk.ht_length, psdu.size());
		if(pk.htsig_ok && mcs == 0) { printf("     HT-SIG ok: MCS%d len=%d cbw=%d\n", pk.mcs, pk.ht_length, pk.ht_40); }
	}
	printf("  -- HT-SIG + CFO/noise (MCS0/3, +-40k, 25 dB) --\n");
	for(int mcs : { 0, 3 }) {
		for(int cfo : { -40000, 40000 }) {
			std::vector<cf> sig = wifi_ht_modulate(psdu, mcs, 0x22);
			std::vector<cf> b = make_burst(sig, cfo, 25, 200, rng);
			wifi_packet pk;
			bool ok = wifi_decode_burst(b.data(), b.size(), 6, pk);
			CHECK(ok && pk.is_ht && pk.htsig_ok && pk.mcs == mcs, "HT MCS%d cfo %d: is_ht %d htsig %d mcs %d", mcs, cfo, pk.is_ht, pk.htsig_ok, pk.mcs);
		}
	}

	printf("\nismmon ofdm selftest: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
