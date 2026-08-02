// ismmon Wi-Fi selftest: build an 802.11 beacon, OFDM-modulate (6 Mbps), add lead/CFO/AWGN, and
// assert the receiver recovers SSID + BSSID + type. Self-consistent (my modulator <-> my demod);
// proves the OFDM chain (STF/LTF sync, chan-est, 64-FFT, Viterbi, descramble, MAC parse). On-air
// convention correctness is confirmed against real captures.
#include "wifi.h"

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
		double sigpow = 0; for(auto& x : sig) { sigpow += std::norm(x); } sigpow /= sig.size();
		double sigma = std::sqrt(sigpow / (2.0 * std::pow(10.0, snr_db / 10.0)));
		std::normal_distribution<float> nd(0.0f, (float)sigma);
		for(auto& x : out) { x += cf(nd(rng), nd(rng)); }
	}
	return out;
}

int main() {
	std::mt19937 rng(7);
	uint8_t bssid[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	std::string ssid = "RFNM-Lime-AP";
	std::vector<uint8_t> psdu = wifi_build_beacon(bssid, ssid);
	printf("  beacon PSDU %zu bytes, ssid='%s'\n", psdu.size(), ssid.c_str());

	// --- clean decode ---
	{
		std::vector<cf> sig = wifi_ofdm_modulate(psdu, 6, 0x5d /*scrambler seed*/);
		std::vector<cf> burst = make_burst(sig, 0, 200, 300, rng);
		wifi_packet pk;
		bool ok = wifi_decode_burst(burst.data(), burst.size(), 6, pk);
		CHECK(ok && pk.sig_ok, "clean: no detect/SIGNAL (ok %d sig_ok %d rate %d len %d)", ok, pk.sig_ok, pk.rate_mbps, pk.length);
		CHECK(pk.rate_mbps == 6, "clean: rate %d != 6", pk.rate_mbps);
		CHECK(pk.length == (int)psdu.size(), "clean: length %d != %zu", pk.length, psdu.size());
		CHECK(pk.mac_ok, "clean: MAC not decoded (FCS?)");
		CHECK(pk.mac_ok && pk.ssid == ssid, "clean: ssid '%s' != '%s'", pk.ssid.c_str(), ssid.c_str());
		CHECK(pk.mac_ok && memcmp(pk.bssid, bssid, 6) == 0, "clean: bssid %s", wifi_mac_string(pk.bssid).c_str());
		CHECK(pk.frame_type == 0x08, "clean: type %d != beacon(8)", pk.frame_type);
		if(pk.mac_ok) { printf("  clean decode: %s '%s' bssid %s rate %d len %d\n",
			pk.type_name.c_str(), pk.ssid.c_str(), wifi_mac_string(pk.bssid).c_str(), pk.rate_mbps, pk.length); }
	}

	// --- CFO + noise ---
	for(double cfo : { -40e3, 0.0, 60e3 }) {
		std::vector<cf> sig = wifi_ofdm_modulate(psdu, 6, 0x37);
		std::vector<cf> burst = make_burst(sig, cfo, 22, 300, rng);
		wifi_packet pk;
		bool ok = wifi_decode_burst(burst.data(), burst.size(), 1, pk);
		CHECK(ok && pk.sig_ok && pk.mac_ok && pk.ssid == ssid,
		      "cfo %.0fk/22dB: ssid='%s' sig_ok %d mac_ok %d", cfo / 1e3, pk.ssid.c_str(), pk.sig_ok, pk.mac_ok);
	}

	// --- SIGNAL header at a rate we don't fully decode (still get rate/length) ---
	{
		std::vector<cf> sig = wifi_ofdm_modulate(psdu, 24, 0x11);   // 24 Mbps: SIGNAL only
		std::vector<cf> burst = make_burst(sig, 0, 200, 300, rng);
		wifi_packet pk;
		wifi_decode_burst(burst.data(), burst.size(), 11, pk);
		CHECK(pk.sig_ok && pk.rate_mbps == 24, "sig-only: rate %d sig_ok %d", pk.rate_mbps, pk.sig_ok);
	}

	// --- sensitivity sweep ---
	printf("  -- sensitivity (60 trials/pt, cfo +30k) --\n");
	for(double snr : { 25.0, 18.0, 14.0, 11.0, 8.0 }) {
		int ok = 0;
		for(int t = 0; t < 60; t++) {
			char s[24]; snprintf(s, sizeof(s), "AP-%d", t);
			std::vector<uint8_t> p = wifi_build_beacon(bssid, s);
			std::vector<cf> sig = wifi_ofdm_modulate(p, 6, 0x5d + t);
			std::vector<cf> burst = make_burst(sig, 30e3, snr, 300, rng);
			wifi_packet pk;
			if(wifi_decode_burst(burst.data(), burst.size(), 6, pk) && pk.mac_ok && pk.ssid == s) { ok++; }
		}
		printf("     SNR %5.1f dB : %2d/60\n", snr, ok);
		if(snr >= 18.0) { CHECK(ok >= 57, "sensitivity low at %.0f dB: %d/60", snr, ok); }
	}

	printf("\nismmon wifi selftest: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
